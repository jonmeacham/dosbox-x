// C_DEBUG-only bounded queue snapshots. Included after PIC's private queue and
// service state declarations; never reads guest memory or changes CPU state.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <limits>
#include <cstring>

extern Bitu cycle_count;
namespace {
struct NativePICQueueTrace {
    bool configured = false, enabled = false, begun = false, finished = false;
    FILE *file = nullptr;
    double start = 0, end = 0;
    uint64_t maximum = 0, rows = 0;
    bool reserved[PIC_QUEUESIZE] = {};
    int free_next[PIC_QUEUESIZE];

    bool configure() {
        if (configured) return enabled;
        configured = true;
        const char *path = std::getenv("DOSBOX_X_NATIVE_PIC_QUEUE_TRACE");
        const char *first = std::getenv("DOSBOX_X_NATIVE_CALLBACK_START_MS");
        const char *last = std::getenv("DOSBOX_X_NATIVE_CALLBACK_END_MS");
        const char *limit = std::getenv("DOSBOX_X_NATIVE_PIC_QUEUE_MAX_ROWS");
        if (!path || !*path || !first || !last || !limit) return false;
        char *tail = nullptr;
        start = std::strtod(first, &tail);
        if (!tail || *tail || !std::isfinite(start) || start < 0) return false;
        end = std::strtod(last, &tail);
        if (!tail || *tail || !std::isfinite(end) || end <= start || end > 600000) return false;
        maximum = std::strtoull(limit, &tail, 10);
        if (!tail || *tail || !maximum || maximum > 1000000) return false;
        file = std::fopen(path, "wx");
        if (!file) return false;
        enabled = true;
        std::fprintf(file, "#DXPQ001 {\"start_ms\":%.17g,\"end_ms\":%.17g,\"max_rows\":%llu,\"capacity\":%u,\"anchor_address\":%llu,\"deadline_digits\":%d,\"deadline_binary_digits\":%d}\n",
            start, end, (unsigned long long)maximum, unsigned(PIC_QUEUESIZE), (unsigned long long)(uintptr_t)&PIC_RunQueue,
            std::numeric_limits<pic_tickindex_t>::max_digits10, std::numeric_limits<pic_tickindex_t>::digits);
        return true;
    }
    static int slot(const PICEntry *entry) {
        if (!entry) return -1;
        const uintptr_t address = (uintptr_t)entry, first = (uintptr_t)&pic_queue.entries[0];
        if (address < first || address >= first + sizeof(pic_queue.entries) || (address - first) % sizeof(PICEntry)) return -2;
        return int((address - first) / sizeof(PICEntry));
    }
    void finish(const char *reason) {
        if (!file || finished) return;
        std::fprintf(file, "#END {\"reason\":\"%s\",\"rows\":%llu,\"pic_ms\":%.17g}\n",
                     reason, (unsigned long long)rows, double(PIC_FullIndex()));
        std::fflush(file); finished = true;
    }
    bool valid_lists() {
        bool seen[PIC_QUEUESIZE] = {};
        unsigned count = 0;
        for (const PICEntry *p = pic_queue.next_entry; p; p = p->next) {
            const int index = slot(p);
            if (index < 0 || seen[index] || ++count > PIC_QUEUESIZE || !std::isfinite(p->index)) return false;
            seen[index] = true;
        }
        for (const PICEntry *p = pic_queue.free_entry; p; p = p->next) {
            const int index = slot(p);
            if (index < 0 || seen[index] || ++count > PIC_QUEUESIZE) return false;
            seen[index] = true;
        }
        for (unsigned i = 0; i < PIC_QUEUESIZE; ++i) {
            if (reserved[i]) { if (seen[i]) return false; ++count; }
        }
        return count == PIC_QUEUESIZE;
    }
    void record(uint64_t scope, const char *kind, const char *phase, PIC_EventHandler handler,
                pic_tickindex_t delay, Bitu value, PICEntry *entry, bool has_cutoff, Bits cutoff, bool initial) {
        if (!valid_lists()) { finish("invalid_queue_or_pool"); return; }
        if (rows >= maximum) { finish("hit_limit"); return; }
        std::fprintf(file,
            "{\"row\":%llu,\"scope\":%llu,\"kind\":\"%s\",\"phase\":\"%s\",\"handler\":%llu,\"delay\":%.*Lg,\"value\":%llu,\"entry_slot\":%d,"
            "\"ticks\":%llu,\"pic_ms\":%.17g,\"tick_index\":%.*Lg,\"cycle_count\":%llu,\"cpu_cycles\":%lld,\"cpu_cycle_left\":%lld,\"cpu_cycle_max\":%lld,\"cpu_io_delay_removed\":%lld,"
            "\"in_service\":%u,\"srv_lag\":%.*Lg,\"free_head\":%d,\"cutoff\":",
            (unsigned long long)rows++, (unsigned long long)scope, kind, phase,
            (unsigned long long)(uintptr_t)handler, std::numeric_limits<pic_tickindex_t>::max_digits10, (long double)delay, (unsigned long long)value, slot(entry),
            (unsigned long long)PIC_Ticks, double(PIC_FullIndex()),
            std::numeric_limits<pic_tickindex_t>::max_digits10, (long double)PIC_TickIndex(), (unsigned long long)cycle_count,
            (long long)CPU_Cycles, (long long)CPU_CycleLeft, (long long)CPU_CycleMax, (long long)CPU_IODelayRemoved,
            unsigned(InEventService), std::numeric_limits<pic_tickindex_t>::max_digits10, (long double)srv_lag, slot(pic_queue.free_entry));
        if (has_cutoff) std::fprintf(file, "%lld", (long long)cutoff); else std::fputs("null", file);
        std::fputs(",\"queue\":[", file); bool first = true;
        for (const PICEntry *p = pic_queue.next_entry; p; p = p->next) {
            std::fprintf(file, "%s{\"slot\":%d,\"handler\":%llu,\"value\":%llu,\"index\":%.*Lg}",
                         first ? "" : ",", slot(p), (unsigned long long)(uintptr_t)p->pic_event,
                         (unsigned long long)p->value, std::numeric_limits<pic_tickindex_t>::max_digits10, (long double)p->index);
            first = false;
        }
        std::fputs("],\"free_order\":", file);
        if (!initial) std::fputs("null", file);
        else {
            std::fputs("[", file); first = true;
            for (const PICEntry *p = pic_queue.free_entry; p; p = p->next) {
                std::fprintf(file, "%s%d", first ? "" : ",", slot(p)); first = false;
            }
            std::fputs("]", file);
        }
        // Exact free-link deltas keep each row bounded without repeating the
        // entire 8192-slot list. -2 denotes a slot absent from the free list.
        int next[PIC_QUEUESIZE];
        for (unsigned i = 0; i < PIC_QUEUESIZE; ++i) next[i] = -2;
        for (const PICEntry *p = pic_queue.free_entry; p; p = p->next) next[slot(p)] = slot(p->next);
        std::fputs(",\"free_changes\":[", file); first = true;
        for (unsigned i = 0; i < PIC_QUEUESIZE; ++i) {
            if (!initial && next[i] != free_next[i]) {
                std::fprintf(file, "%s[%u,%d]", first ? "" : ",", i, next[i]); first = false;
            }
            free_next[i] = next[i];
        }
        std::fputs("]}\n", file);
        if (std::ferror(file)) finish("io_error");
        else if (std::ftell(file) < 0 || std::ftell(file) > 256L * 1024L * 1024L) finish("byte_limit");
    }
    void observe(uint64_t scope, const char *kind, const char *phase, PIC_EventHandler handler,
                 pic_tickindex_t delay, Bitu value, PICEntry *entry, bool has_cutoff, Bits cutoff) {
        if (!configure() || finished) return;
        const double now = double(PIC_FullIndex());
        if (now >= end) { finish("window_complete"); return; }
        if (now < start) return;
        if (!scope) { finish("missing_scope"); return; }
        if (!begun) {
            // With an executing entry the two lists do not cover the pool.
            // Reject that left edge rather than fabricate its reserved slot.
            if (InEventService) { finish("initial_service_in_progress"); return; }
            record(scope, "initial", "state", nullptr, 0, 0, nullptr, false, 0, true);
            begun = true;
        }
        if (!std::strcmp(phase, "cpp_unwind")) { finish("cpp_unwind"); return; }
        if (!std::strcmp(kind, "pic_fire") && (!std::strcmp(phase, "dequeued") || !std::strcmp(phase, "exit"))) {
            const int index = slot(entry);
            if (index < 0) { finish("invalid_reserved_slot"); return; }
            reserved[index] = !std::strcmp(phase, "dequeued");
        }
        if (!finished) record(scope, kind, phase, handler, delay, value, entry, has_cutoff, cutoff, false);
    }
    void epoch_change() {
        if (!configure() || finished) return;
        if (begun || double(PIC_FullIndex()) >= start) finish("epoch_change");
    }
    ~NativePICQueueTrace() { if (file) { finish("process_exit"); std::fclose(file); } }
};
NativePICQueueTrace &native_pic_queue_trace() { static NativePICQueueTrace trace; return trace; }
struct NativePICQueueScope {
    uint64_t scope;
    const char *kind;
    PIC_EventHandler handler;
    pic_tickindex_t delay;
    Bitu value;
    PICEntry *entry;
    bool has_cutoff = false;
    Bits cutoff = 0;
    NativePICQueueScope(const NativeCallbackScope &owner, const char *k, PIC_EventHandler h = nullptr,
                        pic_tickindex_t d = 0, Bitu v = 0, PICEntry *e = nullptr,
                        bool frozen = false, Bits index = 0)
        : scope(owner.observedID()), kind(k), handler(h), delay(d), value(v), entry(e),
          has_cutoff(frozen), cutoff(index) { mark("entry"); }
    void mark(const char *phase) {
        native_pic_queue_trace().observe(scope, kind, phase, handler, delay, value, entry, has_cutoff, cutoff);
    }
    void freeze(Bits index) { has_cutoff = true; cutoff = index; mark("cutoff"); }
    ~NativePICQueueScope() { mark(std::uncaught_exception() ? "cpp_unwind" : "exit"); }
};
}
