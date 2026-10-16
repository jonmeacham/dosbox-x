/*
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "cpu.h"
#include "lazyflags.h"
#include "callback.h"
#include "pic.h"
#include "fpu.h"
#include "paging.h"
#include "mmx.h"

extern bool do_lds_wraparound;

bool CPU_RDMSR();
bool CPU_WRMSR();
bool CPU_SYSENTER();
bool CPU_SYSEXIT();

#define PRE_EXCEPTION { }

#define CPU_CORE CPU_ARCHTYPE_386

#define DoString DoString_Normal

static uint16_t last_ea86_offset;

extern bool ignore_opcode_63;

#if C_DEBUG
#include "debug.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <deque>
#include <string>
#include <utility>
#include <vector>

extern Bitu cycle_count;
namespace {
// Diagnostic only: preserve the normal decoder's actual one-unit loop debit,
// including all additional REP/I/O/control effects inside each instruction.
// Raw EFLAGS are retained without materializing or changing lazy arithmetic flags.
struct NativeInstructionTrace {
    bool configured = false, active = false, finished = false;
    FILE *file = nullptr;
    double start = 0, end = 0;
    double trigger_duration = 0;
    uint64_t trigger_address = 0;
    uint64_t trigger_eax_mask = 0, trigger_eax_value = 0;
    bool triggered = false;
    uint64_t limit = 0, instructions = 0, rows = 0, runs = 0;
    uint64_t lookbehind = 0, history_count = 0, history_bytes = 0, first_id = 1, first_row = 0;
    uint64_t trigger_counter = 0, retained_history = 0;
    double trigger_pic = 0;
    struct Pending {
        uint64_t first_id = 0, first_row = 0, instructions = 0;
        double pic = 0;
        std::string text;
    } pending;
    std::deque<Pending> history;
    std::vector<uint64_t> stack;

    bool configure() {
        if (configured) return active;
        configured = true;
        const char *path = std::getenv("DOSBOX_X_NATIVE_INSTRUCTION_TRACE");
        const char *first = std::getenv("DOSBOX_X_NATIVE_INSTRUCTION_START_MS");
        const char *last = std::getenv("DOSBOX_X_NATIVE_INSTRUCTION_END_MS");
        const char *maximum = std::getenv("DOSBOX_X_NATIVE_INSTRUCTION_MAX");
        const char *trigger = std::getenv("DOSBOX_X_NATIVE_INSTRUCTION_TRIGGER_ADDRESS");
        const char *duration = std::getenv("DOSBOX_X_NATIVE_INSTRUCTION_TRIGGER_DURATION_MS");
        const char *eax_mask = std::getenv("DOSBOX_X_NATIVE_INSTRUCTION_TRIGGER_EAX_MASK");
        const char *eax_value = std::getenv("DOSBOX_X_NATIVE_INSTRUCTION_TRIGGER_EAX_VALUE");
        const char *history_limit = std::getenv("DOSBOX_X_NATIVE_INSTRUCTION_LOOKBEHIND");
        if (!path || !*path || !first || !last || !maximum) return false;
        char *tail = nullptr;
        start = std::strtod(first, &tail);
        if (!tail || *tail || !std::isfinite(start) || start < 0) return false;
        end = std::strtod(last, &tail);
        if (!tail || *tail || !std::isfinite(end) || end <= start || end > 600000) return false;
        if (trigger || duration) {
            if (!trigger || !*trigger || !duration || !*duration) return false;
            trigger_address = std::strtoull(trigger, &tail, 0);
            if (!tail || *tail || !trigger_address || trigger_address > UINT32_MAX) return false;
            trigger_duration = std::strtod(duration, &tail);
            if (!tail || *tail || !std::isfinite(trigger_duration) || trigger_duration <= 0 || trigger_duration > 1000 || end + trigger_duration > 600000) return false;
        } else if (end - start > 1000) return false;
        if (eax_mask || eax_value) {
            if (!trigger_address || !eax_mask || !*eax_mask || !eax_value || !*eax_value) return false;
            trigger_eax_mask = std::strtoull(eax_mask, &tail, 0);
            if (!tail || *tail || !trigger_eax_mask || trigger_eax_mask > UINT32_MAX) return false;
            trigger_eax_value = std::strtoull(eax_value, &tail, 0);
            if (!tail || *tail || trigger_eax_value > UINT32_MAX || (trigger_eax_value & ~trigger_eax_mask)) return false;
        }
        limit = std::strtoull(maximum, &tail, 10);
        if (!tail || *tail || !limit || limit > 1000000) return false;
        if (history_limit) {
            lookbehind = std::strtoull(history_limit, &tail, 10);
            if (!trigger_address || !tail || *tail || !lookbehind || lookbehind > 16384 || lookbehind >= limit) return false;
        }
        file = std::fopen(path, "wx");
        if (!file) return false;
        if (trigger_address) {
            std::fprintf(file, "#DXARM001 {\"start_ms\":%.17g,\"end_ms\":%.17g,\"address\":%llu,\"duration_ms\":%.17g",
                         start, end, (unsigned long long)trigger_address, trigger_duration);
            if (trigger_eax_mask) std::fprintf(file, ",\"eax_mask\":%llu,\"eax_value\":%llu",
                (unsigned long long)trigger_eax_mask, (unsigned long long)trigger_eax_value);
            if (lookbehind) std::fprintf(file, ",\"lookbehind_instructions\":%llu", (unsigned long long)lookbehind);
            std::fputs("}\n", file);
        } else write_header();
        active = !std::ferror(file);
        return active;
    }
    void write_header() {
        std::fprintf(file, "#DXINS001 {\"start_ms\":%.17g,\"end_ms\":%.17g,\"maximum_instructions\":%llu,\"entry_after_loop_debit\":1",
                     start, end, (unsigned long long)limit);
        if (lookbehind) std::fprintf(file, ",\"first_id\":%llu,\"first_row\":%llu,\"history_instructions\":%llu,\"trigger_counter\":%llu,\"trigger_pic_ms\":%.17g",
            (unsigned long long)first_id, (unsigned long long)first_row, (unsigned long long)retained_history,
            (unsigned long long)trigger_counter, trigger_pic);
        std::fputs("}\n", file);
        std::fprintf(file, "row,id,parent,run,phase,pic_ms,cycle_count,cpu_cycles,cpu_cycle_left,cpu_cycle_max,cpu_io_delay_removed,cs,cs_base,eip,ss,ss_base,esp,stack_big,stack_mask,stack_notmask,code_big,pmode,cpl,if,raw_flags,eax,ebx,ecx,edx,esi,edi,ebp,ds,es,opcode_status,opcode_hex,outcome\n");
    }
    // Buffer whole outer decoder instructions, including nested scopes and
    // auxiliary observations. Never discard a partial instruction group.
    template <typename... Args> void emit(const char *format, Args... args) {
        if (!lookbehind || triggered) { std::fprintf(file, format, args...); return; }
        char line[2048];
        const int length = std::snprintf(line, sizeof(line), format, args...);
        if (length < 0 || size_t(length) >= sizeof(line) || pending.text.size() + size_t(length) > lookbehind * 4096) {
            finish("history_byte_limit"); return;
        }
        pending.text.append(line, size_t(length));
    }
    void begin_pending() {
        if (!lookbehind || triggered || !stack.empty()) return;
        pending = Pending();
        pending.first_id = instructions + 1; pending.first_row = rows;
        pending.pic = double(PIC_FullIndex());
    }
    void complete_pending() {
        if (!lookbehind || triggered || finished || !stack.empty()) return;
        pending.instructions = instructions + 1 - pending.first_id;
        if (pending.instructions > lookbehind) { finish("history_instruction_limit"); return; }
        history_count += pending.instructions;
        history_bytes += pending.text.size();
        history.push_back(std::move(pending));
        while (history_count > lookbehind || history_bytes > lookbehind * 4096) {
            history_count -= history.front().instructions;
            history_bytes -= history.front().text.size(); history.pop_front();
        }
    }
    bool trigger(double now) {
        if (!stack.empty()) { finish("trigger_inside_instruction"); return false; }
        triggered = true; trigger_pic = now; trigger_counter = cycle_count;
        first_id = history.empty() ? instructions + 1 : history.front().first_id;
        first_row = history.empty() ? rows : history.front().first_row;
        retained_history = history_count;
        start = history.empty() ? now : history.front().pic; end = now + trigger_duration;
        if (end - start > 1000) { finish("history_time_limit"); return false; }
        write_header();
        for (const auto &group : history) std::fwrite(group.text.data(), 1, group.text.size(), file);
        history.clear(); history_count = 0; history_bytes = 0;
        if (std::ferror(file)) { finish("io_error"); return false; }
        return true;
    }
    void opl_read(Bitu port, Bitu width) {
        if (!active || finished || stack.empty()) return;
        emit("#DXOPL001 {\"id\":%llu,\"cycle_count\":%llu,\"pic_ms\":%.17g,\"cpu_cycles\":%lld,\"cpu_cycle_left\":%lld,\"cpu_cycle_max\":%lld,\"cpu_io_delay_removed\":%lld,\"port\":%u,\"width\":%u}\n",
            (unsigned long long)stack.back(), (unsigned long long)cycle_count, double(PIC_FullIndex()),
            (long long)CPU_Cycles, (long long)CPU_CycleLeft, (long long)CPU_CycleMax, (long long)CPU_IODelayRemoved,
            unsigned(port), unsigned(width));
        if (std::ferror(file)) finish("io_error");
    }
    void cmps(unsigned size, Bitu count, PhysPt source, PhysPt destination,
              uint32_t si, uint32_t di, uint32_t mask, Bits direction, bool repeat, bool equal) {
        if (!active || finished || stack.empty()) return;
        const Bitu bound = std::min(count, Bitu(CPU_Cycles > 0 ? CPU_Cycles : 1));
        const int step = int(direction) * int(size);
        char source_hex[513] = {}, destination_hex[513] = {};
        const char *status = "ok";
        static const char digits[] = "0123456789abcdef";
        if (bound > 64 || size > 4) status = "iteration_bound";
        else for (Bitu i = 0; i < bound; ++i) {
            for (unsigned j = 0; j < size; ++j) {
                const uint64_t sa = uint64_t(source) + ((si + uint32_t(int64_t(step) * int64_t(i))) & mask) + j;
                const uint64_t da = uint64_t(destination) + ((di + uint32_t(int64_t(step) * int64_t(i))) & mask) + j;
                uint8_t sv = 0, dv = 0;
                if (sa > UINT32_MAX || da > UINT32_MAX || mem_readb_checked(LinearPt(sa), &sv) || mem_readb_checked(LinearPt(da), &dv)) {
                    status = "unreadable"; break;
                }
                const Bitu index = (i * size + j) * 2;
                source_hex[index] = digits[sv >> 4]; source_hex[index + 1] = digits[sv & 15];
                destination_hex[index] = digits[dv >> 4]; destination_hex[index + 1] = digits[dv & 15];
            }
            if (status[0] != 'o') break;
        }
        if (status[0] != 'o') source_hex[0] = destination_hex[0] = 0;
        emit("#DXCMP001 {\"id\":%llu,\"cycle_count\":%llu,\"cpu_cycles\":%lld,\"cpu_cycle_left\":%lld,\"cpu_cycle_max\":%lld,\"cpu_io_delay_removed\":%lld,\"operand_size\":%u,\"count\":%llu,\"maximum_iterations\":%llu,\"source_base\":%u,\"destination_base\":%u,\"si\":%u,\"di\":%u,\"address_mask\":%u,\"step\":%d,\"repeat\":%u,\"repeat_equal\":%u,\"status\":\"%s\",\"source_hex\":\"%s\",\"destination_hex\":\"%s\"}\n",
            (unsigned long long)stack.back(), (unsigned long long)cycle_count,
            (long long)CPU_Cycles, (long long)CPU_CycleLeft, (long long)CPU_CycleMax, (long long)CPU_IODelayRemoved,
            size, (unsigned long long)count, (unsigned long long)bound, unsigned(source), unsigned(destination),
            si, di, mask, step, unsigned(repeat), unsigned(equal), status, source_hex, destination_hex);
        if (std::ferror(file)) finish("io_error");
    }
    void finish(const char *reason) {
        if (!file || finished) return;
        finished = true; active = false;
        std::fprintf(file, "#DXINEND1 {\"reason\":\"%s\",\"rows\":%llu,\"instructions\":%llu,\"open\":%llu,\"pic_ms\":%.17g}\n",
                     reason, (unsigned long long)(rows - first_row), (unsigned long long)(instructions + 1 - first_id),
                     (unsigned long long)stack.size(), double(PIC_FullIndex()));
        std::fflush(file);
    }
    ~NativeInstructionTrace() {
        if (file) { finish("process_exit"); std::fclose(file); }
    }
    void record(uint64_t id, uint64_t parent, uint64_t run, unsigned phase, const char *outcome) {
        if (!file || finished) return;
        char hex[33] = {};
        const char *status = "exit";
        if (!phase) {
            status = "ok";
            const uint64_t address = uint64_t(SegPhys(cs)) + uint64_t(reg_eip);
            static const char digits[] = "0123456789abcdef";
            for (unsigned i = 0; i < 16; ++i) {
                uint8_t value = 0;
                if (address + i > UINT32_MAX || mem_readb_checked(LinearPt(address + i), &value)) {
                    status = "unreadable"; hex[0] = 0; break;
                }
                hex[i * 2] = digits[value >> 4]; hex[i * 2 + 1] = digits[value & 15];
            }
        }
        emit("%llu,%llu,%llu,%llu,%u,%.17g,%llu,%lld,%lld,%lld,%lld,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%s,%s,%s\n",
            (unsigned long long)rows++, (unsigned long long)id, (unsigned long long)parent, (unsigned long long)run,
            phase, double(PIC_FullIndex()), (unsigned long long)cycle_count,
            (long long)CPU_Cycles, (long long)CPU_CycleLeft, (long long)CPU_CycleMax, (long long)CPU_IODelayRemoved,
            unsigned(SegValue(cs)), unsigned(SegPhys(cs)), unsigned(reg_eip), unsigned(SegValue(ss)), unsigned(SegPhys(ss)), unsigned(reg_esp),
            unsigned(cpu.stack.big), unsigned(cpu.stack.mask), unsigned(cpu.stack.notmask), unsigned(cpu.code.big), unsigned(cpu.pmode), unsigned(cpu.cpl),
            unsigned((reg_flags & FLAG_IF) != 0), unsigned(reg_flags), unsigned(reg_eax), unsigned(reg_ebx), unsigned(reg_ecx), unsigned(reg_edx),
            unsigned(reg_esi), unsigned(reg_edi), unsigned(reg_ebp), unsigned(SegValue(ds)), unsigned(SegValue(es)), status, hex, outcome);
        if (std::ferror(file)) finish("io_error");
    }
} native_instruction_trace;

struct NativeInstructionRun {
    uint64_t id = 0;
    NativeInstructionRun() {
        if (native_instruction_trace.configure()) id = ++native_instruction_trace.runs;
    }
};
struct NativeInstructionScope {
    uint64_t id = 0, parent = 0, run = 0;
    explicit NativeInstructionScope(uint64_t run_id) {
        auto &trace = native_instruction_trace;
        if (!trace.active) return;
        const double now = double(PIC_FullIndex());
        if (trace.trigger_address && !trace.triggered) {
            if (now < trace.start) return;
            if (now >= trace.end) { trace.finish("trigger_not_reached"); return; }
            const bool hit = uint64_t(SegPhys(cs)) + uint64_t(reg_eip) == trace.trigger_address &&
                (!trace.trigger_eax_mask || (uint64_t(reg_eax) & trace.trigger_eax_mask) == trace.trigger_eax_value);
            if (hit) { if (!trace.trigger(now)) return; }
            else if (!trace.lookbehind) return;
        }
        if (now < trace.start) return;
        if (now >= trace.end && trace.stack.empty()) { trace.finish("window_complete"); return; }
        if ((!trace.trigger_address || trace.triggered) && trace.instructions + 1 - trace.first_id >= trace.limit) { trace.finish("instruction_limit"); return; }
        trace.begin_pending();
        id = ++trace.instructions; run = run_id;
        parent = trace.stack.empty() ? 0 : trace.stack.back();
        trace.stack.push_back(id);
        trace.record(id, parent, run, 0, "entry");
    }
    ~NativeInstructionScope() {
        if (!id) return;
        auto &trace = native_instruction_trace;
        trace.record(id, parent, run, 1, std::uncaught_exception() ? "cpp_unwind" : "normal");
        if (!trace.stack.empty()) trace.stack.pop_back();
        trace.complete_pending();
    }
};
}

void NativeInstructionTraceOPLRead(Bitu port, Bitu width) {
    native_instruction_trace.opl_read(port, width);
}
#endif

#define LoadMb(off) mem_readb_inline(off)
#define LoadMw(off) mem_readw_inline(off)
#define LoadMd(off) mem_readd_inline(off)
#define LoadMq(off) (((uint64_t)mem_readd_inline(off+4)<<(uint64_t)32) | (uint64_t)mem_readd_inline(off))
#define SaveMb(off,val)	mem_writeb_inline(off,val)
#define SaveMw(off,val)	mem_writew_inline(off,val)
#define SaveMd(off,val)	mem_writed_inline(off,val)
#define SaveMq(off,val) {mem_writed_inline(off,((uint32_t)(val))&0xffffffff);mem_writed_inline(off+4,(((uint64_t)(val))>>((uint64_t)32))&0xffffffff);}

Bitu cycle_count;

#if C_FPU
#define CPU_FPU	1u						//Enable FPU escape instructions
#endif

#define CPU_PIC_CHECK 1u
#define CPU_TRAP_CHECK 1u

#define CPU_TRAP_DECODER	CPU_Core_Normal_Trap_Run

#define OPCODE_NONE			0x000u
#define OPCODE_0F			0x100u
#define OPCODE_SIZE			0x200u

#define PREFIX_ADDR			0x1u
#define PREFIX_REP			0x2u

#define TEST_PREFIX_ADDR	(core.prefixes & PREFIX_ADDR)
#define TEST_PREFIX_REP		(core.prefixes & PREFIX_REP)

#define DO_PREFIX_SEG(_SEG)					\
	BaseDS=SegBase(_SEG);					\
	BaseSS=SegBase(_SEG);					\
	core.base_val_ds=_SEG;					\
	goto restart_opcode;

#define DO_PREFIX_ADDR()								\
	core.prefixes=(core.prefixes & ~PREFIX_ADDR) |		\
	(cpu.code.big ^ PREFIX_ADDR);						\
	core.ea_table=&EATable[(core.prefixes&1u) * 256u];	\
	goto restart_opcode;

#define DO_PREFIX_REP(_ZERO)				\
	core.prefixes|=PREFIX_REP;				\
	core.rep_zero=_ZERO;					\
	goto restart_opcode;

#define REMEMBER_PREFIX(_x) last_prefix = (_x)

static uint8_t last_prefix;

typedef PhysPt (*GetEAHandler)(void);

static const uint32_t AddrMaskTable[2]={0x0000ffffu,0xffffffffu};

static struct {
	Bitu opcode_index;
	PhysPt cseip;
	PhysPt base_ds,base_ss;
	SegNames base_val_ds;
	bool rep_zero;
	Bitu prefixes;
	GetEAHandler * ea_table;
} core;

/* FIXME: Someone at Microsoft tell how subtracting PhysPt - PhysPt = __int64, or PhysPt + PhysPt = __int64 */
#define GETIP		((PhysPt)(core.cseip-SegBase(cs)))
#define SAVEIP		reg_eip=GETIP;
#define LOADIP		core.cseip=((PhysPt)(SegBase(cs)+reg_eip));

#define SegBase(c)	SegPhys(c)
#define BaseDS		core.base_ds
#define BaseSS		core.base_ss

static INLINE void FetchDiscardb() {
	core.cseip+=1;
}

static INLINE uint8_t FetchPeekb() {
	uint8_t temp=LoadMb(core.cseip);
	return temp;
}

static INLINE uint8_t Fetchb() {
	uint8_t temp=LoadMb(core.cseip);
	core.cseip+=1;
	return temp;
}

static INLINE uint16_t Fetchw() {
	uint16_t temp=LoadMw(core.cseip);
	core.cseip+=2;
	return temp;
}
static INLINE uint32_t Fetchd() {
	uint32_t temp=LoadMd(core.cseip);
	core.cseip+=4;
	return temp;
}

#define Push_16 CPU_Push16
#define Push_32 CPU_Push32
#define Pop_16 CPU_Pop16
#define Pop_32 CPU_Pop32

#include "instructions.h"
#include "core_normal/support.h"
#if C_DEBUG
#define ALBION_NATIVE_NORMAL_INSTRUCTION_TRACE 1
#endif
#include "core_normal/string.h"
#undef ALBION_NATIVE_NORMAL_INSTRUCTION_TRACE


#define EALookupTable (core.ea_table)

Bits CPU_Core_Normal_Run(void) {
	if (CPU_Cycles <= 0)
		return CBRET_NONE;

#if C_DEBUG
    const NativeInstructionRun instruction_run;
#endif

	while (CPU_Cycles-->0) {
		LOADIP;
		last_prefix=MP_NONE;
		core.opcode_index=cpu.code.big*(Bitu)0x200u;
		core.prefixes=cpu.code.big;
		last_ea86_offset=0;
		core.ea_table=&EATable[cpu.code.big*256u];
		BaseDS=SegBase(ds);
		BaseSS=SegBase(ss);
		core.base_val_ds=ds;
#if C_DEBUG
#if C_HEAVY_DEBUG
		if (DEBUG_HeavyIsBreakpoint()) {
			FillFlags();
			return (Bits)debugCallback;
		}
#endif
#endif
		cycle_count++;
#if C_DEBUG
        const NativeInstructionScope instruction_scope(instruction_run.id);
#endif
restart_opcode:
		switch (core.opcode_index+Fetchb()) {
		#include "core_normal/prefix_none.h"
		#include "core_normal/prefix_0f.h"
		#include "core_normal/prefix_66.h"
		#include "core_normal/prefix_66_0f.h"
		default:
		illegal_opcode:
#if C_DEBUG	
			{
				bool ignore=false;
				Bitu len=(GETIP-reg_eip);
				LOADIP;
				if (len>16) len=16;
				char tempcode[16*2+1];char * writecode=tempcode;
				if (ignore_opcode_63 && mem_readb(core.cseip) == 0x63)
					ignore = true;
				for (;len>0;len--) {
					sprintf(writecode,"%02X",mem_readb(core.cseip++));
					writecode+=2;
				}
				if (!ignore)
					LOG(LOG_CPU,LOG_NORMAL)("Illegal/Unhandled opcode %s",tempcode);
			}
#endif
			CPU_Exception(6,0);
			continue;
		gp_fault:
			LOG_MSG("Segment limit violation");
			CPU_Exception(EXCEPTION_GP,0);
			continue;
		}
		SAVEIP;
#if C_DEBUG
        // POPF/POPFD can leave the decoder with a pending PIC/trap check.
        // Keep instruction_scope alive until EIP and flags are finalized;
        // an outside-loop label would destroy it before SAVEIP/FillFlags.
        continue;
decode_end:
        SAVEIP;
        FillFlags();
        return CBRET_NONE;
#endif
	}
#if C_DEBUG
    {
        NativeCallbackScope flags_trace("decoder_flags");
        FillFlags();
    }
#else
    FillFlags();
#endif
    return CBRET_NONE;
#if !C_DEBUG
decode_end:
	SAVEIP;
	FillFlags();
	return CBRET_NONE;
#endif
}

Bits CPU_Core_Normal_Trap_Run(void) {
	Bits oldCycles = CPU_Cycles;
	CPU_Cycles = 1;
	cpu.trap_skip = false;

	Bits ret=CPU_Core_Normal_Run();
	if (!cpu.trap_skip) CPU_DebugException(DBINT_STEP,reg_eip);
	CPU_Cycles = oldCycles-1;
	cpudecoder = &CPU_Core_Normal_Run;

	return ret;
}



void CPU_Core_Normal_Init(void) {

}

