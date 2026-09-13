/* Opt-in guest input and read-only observation, polled by the normal CPU loop.
 * No debugger entry, guest memory writes, or execution-state manipulation. */
#include "guest_control.h"
#if defined(__unix__) || defined(__APPLE__)
#include "dosbox.h"
#include "keyboard.h"
#include "mouse.h"
#include "pic.h"
#include "mem.h"
#include "paging.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <deque>
#include <map>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr size_t MAX_LINE = 512, MAX_PENDING = 128, MAX_OUT = 65536;
constexpr uint32_t MAX_DELAY = 600000, MAX_DUMP = 16 * 1024 * 1024;
struct Event {
    enum Kind { Key, Move, Button, Dump } kind = Move;
    uint64_t id = 0, session = 0;
    uint32_t delay = 0, duration = 0, address = 0, length = 0;
    KBD_KEYS key = KBD_NONE;
    int x = 0, y = 0, button = 0;
    bool pressed = false, started = false;
    std::string command, file;
};
struct State {
    bool initialized = false, disabled = false, failed = false, discarding = false;
    bool socket_created = false;
    int listener = -1, client = -1, directory = -1, log = -1;
    uint64_t next = 1, session = 0;
    std::string socket_path, input;
    std::deque<std::string> output;
    size_t output_bytes = 0;
    std::map<uint64_t, Event> events;
    std::set<KBD_KEYS> reserved_keys, pressed_keys;
    std::set<int> pressed_buttons;
} s;

bool write_all(int fd, const char *data, size_t length)
{
    while (length) {
        const ssize_t n = write(fd, data, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n;
        length -= size_t(n);
    }
    return true;
}
std::string quote(const std::string &value)
{
    static const char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
        else if (c < 32 || c >= 127) {
            result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15];
        } else result += char(c);
    }
    return result + '"';
}
void disconnect()
{
    if (s.client >= 0) close(s.client);
    s.client = -1;
    s.input.clear();
    s.discarding = false;
    s.output.clear();
    s.output_bytes = 0;
}
void record(const Event &e, const char *status, const char *reason = "")
{
    std::ostringstream out;
    out.precision(17);
    out << "{\"receipt\":" << e.id << ",\"session\":" << e.session
        << ",\"pid\":" << getpid() << ",\"status\":" << quote(status)
        << ",\"guestTick\":" << PIC_FullIndex()
        << ",\"command\":" << quote(e.command)
        << ",\"reason\":" << quote(reason) << "}\n";
    const auto line = out.str();
    if (s.log < 0 || !write_all(s.log, line.data(), line.size())) {
        s.failed = true;
        return;
    }
    if (s.client < 0 || e.session != s.session) return;
    if (s.output_bytes + line.size() > MAX_OUT) { disconnect(); return; }
    s.output.push_back(line);
    s.output_bytes += line.size();
}
bool number(const std::string &text, long long &value, int base = 10)
{
    if (text.empty() || text.find('\0') != std::string::npos) return false;
    char *end = nullptr;
    errno = 0;
    value = strtoll(text.c_str(), &end, base);
    return end != text.c_str() && *end == 0 && errno != ERANGE;
}
static bool keyname(const std::string &n,KBD_KEYS &k) {
 static const std::map<std::string,KBD_KEYS> m={{"escape",KBD_esc},{"esc",KBD_esc},{"enter",KBD_enter},{"space",KBD_space},{"tab",KBD_tab},{"backspace",KBD_backspace},{"up",KBD_up},{"down",KBD_down},{"left",KBD_left},{"right",KBD_right},{"insert",KBD_insert},{"delete",KBD_delete},{"home",KBD_home},{"end",KBD_end},{"pageup",KBD_pageup},{"pagedown",KBD_pagedown}};
 auto it=m.find(n); if(it!=m.end()){k=it->second;return true;} if(n.size()==1){ char c=n[0]; if(c>='a'&&c<='z'){ static const KBD_KEYS letters[]={KBD_a,KBD_b,KBD_c,KBD_d,KBD_e,KBD_f,KBD_g,KBD_h,KBD_i,KBD_j,KBD_k,KBD_l,KBD_m,KBD_n,KBD_o,KBD_p,KBD_q,KBD_r,KBD_s,KBD_t,KBD_u,KBD_v,KBD_w,KBD_x,KBD_y,KBD_z}; k=letters[c-'a'];return true;} if(c>='0'&&c<='9'){ static const KBD_KEYS d[]={KBD_0,KBD_1,KBD_2,KBD_3,KBD_4,KBD_5,KBD_6,KBD_7,KBD_8,KBD_9}; k=d[c-'0'];return true;} } return false;
}

bool parse(const std::string &line, Event &e)
{
    if (line.size() > MAX_LINE || line.find('\0') != std::string::npos) return false;
    std::istringstream input(line);
    std::vector<std::string> t;
    std::string token;
    while (input >> token) t.push_back(token);
    long long delay = 0, duration = 0, x = 0, y = 0;
    if (t.size() == 4 && t[0] == "KEY") {
        if (!number(t[1], delay) || !number(t[2], duration) ||
            delay < 0 || delay > MAX_DELAY || duration < 1 ||
            duration > MAX_DELAY || delay > MAX_DELAY - duration ||
            !keyname(t[3], e.key)) return false;
        e.kind = Event::Key;
        e.duration = uint32_t(duration);
    } else if (t.size() == 5 && t[0] == "MOUSE") {
        if (!number(t[2], delay) || delay < 0 || delay > MAX_DELAY) return false;
        if (t[1] == "MOVE") {
            if (!number(t[3], x) || !number(t[4], y) ||
                x < -32768 || x > 32767 || y < -32768 || y > 32767) return false;
            e.kind = Event::Move; e.x = int(x); e.y = int(y);
        } else if (t[1] == "BUTTON") {
            if (t[3] != "left" && t[3] != "right" && t[3] != "middle") return false;
            if (t[4] != "press" && t[4] != "release") return false;
            e.kind = Event::Button;
            e.button = t[3] == "left" ? 0 : t[3] == "right" ? 1 : 2;
            e.pressed = t[4] == "press";
        } else return false;
    } else if (t.size() == 5 && t[0] == "MEMDUMP") {
        if (!number(t[1], delay) || delay < 0 || delay > MAX_DELAY ||
            !number(t[2], x, 16) || !number(t[3], y, 16) ||
            x < 0 || x > MAX_DUMP || y < 1 || y > MAX_DUMP || x > MAX_DUMP - y)
            return false;
        if (t[4].empty() || t[4].size() > 128 || t[4][0] == '.' ||
            t[4].find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos)
            return false;
        e.kind = Event::Dump; e.address = uint32_t(x); e.length = uint32_t(y); e.file = t[4];
    } else return false;
    e.delay = uint32_t(delay);
    return true;
}

const char *dump(const Event &e)
{
    const int fd = openat(s.directory, e.file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return "open";
    const char *error = nullptr;
    char buffer[4096];
    for (uint32_t offset = 0; offset < e.length && !error;) {
        const size_t count = std::min<size_t>(sizeof(buffer), e.length - offset);
        for (size_t i = 0; i < count; ++i) {
            uint8_t value = 0;
            if (mem_readb_checked(LinearPt(e.address + offset + i), &value)) { error = "read"; break; }
            buffer[i] = char(value);
        }
        if (!error && !write_all(fd, buffer, count)) error = "write";
        offset += uint32_t(count);
    }
    if (close(fd) != 0 && !error) error = "close";
    if (error) unlinkat(s.directory, e.file.c_str(), 0);
    return error;
}
void callback(Bitu value)
{
    auto found = s.events.find(uint64_t(value));
    if (found == s.events.end() || s.failed || s.disabled) return;
    Event &e = found->second;
    const char *error = nullptr;
    switch (e.kind) {
    case Event::Key:
        if (!e.started) {
            KEYBOARD_AddKey(e.key, true);
            s.pressed_keys.insert(e.key);
            e.started = true;
            record(e, "pressed");
            PIC_AddEvent(callback, double(e.duration), value);
            return;
        }
        KEYBOARD_AddKey(e.key, false);
        s.pressed_keys.erase(e.key);
        s.reserved_keys.erase(e.key);
        break;
    case Event::Move:
        Mouse_CursorInjected(float(e.x), float(e.y), 0, 0, true);
        break;
    case Event::Button:
        if (e.pressed) { Mouse_ButtonPressed(uint8_t(e.button)); s.pressed_buttons.insert(e.button); }
        else { Mouse_ButtonReleased(uint8_t(e.button)); s.pressed_buttons.erase(e.button); }
        break;
    case Event::Dump:
        error = dump(e);
        break;
    }
    record(e, error ? "error" : "done", error ? error : "");
    s.events.erase(found);
}
void schedule(const std::string &line, const char *forced_error = nullptr)
{
    Event e;
    e.id = s.next;
    e.session = s.session;
    e.command = line;
    // PIC transports IDs in Bitu; fail closed before truncation or wraparound.
    if (s.next == uint64_t(std::numeric_limits<Bitu>::max())) {
        record(e, "rejected", "receipt_limit");
        s.failed = true;
        return;
    }
    ++s.next;
    const char *error = forced_error;
    if (!error && !parse(line, e)) error = "syntax_or_range";
    if (!error && s.events.size() >= MAX_PENDING) error = "queue_full";
    if (!error && e.kind == Event::Key && s.reserved_keys.count(e.key)) error = "key_pending";
    if (error) { record(e, "rejected", error); return; }
    record(e, "enqueued");
    if (s.failed) return;
    s.events.emplace(e.id, e);
    if (e.kind == Event::Key) s.reserved_keys.insert(e.key);
    PIC_AddEvent(callback, double(e.delay), Bitu(e.id));
}
bool nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return false;
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) return false;
#endif
    return true;
}
void init()
{
    s.initialized = true;
    const char *path = getenv("DOSBOX_X_GUEST_CONTROL_SOCKET");
    const char *dir = getenv("DOSBOX_X_GUEST_CONTROL_DIR");
    if (!path && !dir) { s.disabled = true; return; }
    if (!path || !dir || path[0] != '/' || dir[0] != '/' ||
        strlen(path) >= sizeof(sockaddr_un::sun_path)) { s.disabled = true; return; }
    s.socket_path = path;
    s.directory = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (s.directory < 0) { GuestControl_Shutdown(); return; }
    s.log = openat(s.directory, "guest-control.log", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (s.log < 0) { GuestControl_Shutdown(); return; }
    s.listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s.listener < 0 || !nonblocking(s.listener)) { GuestControl_Shutdown(); return; }
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    const mode_t previous = umask(077);
    const int result = bind(s.listener, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    umask(previous);
    if (result < 0) { GuestControl_Shutdown(); return; }
    s.socket_created = true;
    if (chmod(path, 0600) < 0) { GuestControl_Shutdown(); return; }
    if (listen(s.listener, 1) < 0) { GuestControl_Shutdown(); return; }
    Event ready;
    ready.command = "guest-control-v1";
    record(ready, "ready");
    if (s.failed) { GuestControl_Shutdown(); return; }
    atexit(GuestControl_Shutdown);
}
} // namespace

void GuestControl_Poll()
{
    if (!s.initialized) init();
    if (s.disabled) return;
    if (s.failed) { GuestControl_Shutdown(); return; }
    if (s.client < 0) {
        const int fd = accept(s.listener, nullptr, nullptr);
        if (fd >= 0) {
            if (!nonblocking(fd)) close(fd);
            else { s.client = fd; ++s.session; }
        }
    }
    if (s.client < 0) return;
    // A single bounded read is parsed bytewise: overlong line suffixes are discarded.
    char buffer[256];
    const ssize_t count = recv(s.client, buffer, sizeof(buffer), 0);
    if (count == 0 || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        disconnect(); return;
    }
    for (ssize_t i = 0; i < count && s.client >= 0 && !s.failed; ++i) {
        const char c = buffer[i];
        if (s.discarding) { if (c == '\n') s.discarding = false; continue; }
        if (c == '\n') {
            if (!s.input.empty() && s.input.back() == '\r') s.input.pop_back();
            const std::string line = s.input;
            s.input.clear();
            schedule(line);
        } else if (s.input.size() == MAX_LINE) {
            schedule(s.input, "line_too_long");
            s.input.clear();
            s.discarding = true;
        } else s.input += c;
    }
    size_t budget = 4096;
    while (s.client >= 0 && !s.output.empty() && budget) {
        auto &line = s.output.front();
        const size_t wanted = std::min(budget, line.size());
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
        const ssize_t n = send(s.client, line.data(), wanted, flags);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
        if (n <= 0) { disconnect(); break; }
        budget -= size_t(n); s.output_bytes -= size_t(n);
        if (size_t(n) == line.size()) s.output.pop_front();
        else line.erase(0, size_t(n));
    }
    if (s.failed) GuestControl_Shutdown();
}
void GuestControl_Shutdown()
{
    if (s.disabled && s.listener < 0 && s.directory < 0) return;
    s.disabled = true;
    PIC_RemoveEvents(callback);
    for (auto key : s.pressed_keys) KEYBOARD_AddKey(key, false);
    for (auto button : s.pressed_buttons) Mouse_ButtonReleased(uint8_t(button));
    for (const auto &entry : s.events) record(entry.second, "cancelled", "shutdown");
    s.events.clear(); s.reserved_keys.clear(); s.pressed_keys.clear(); s.pressed_buttons.clear();
    disconnect();
    if (s.listener >= 0) close(s.listener);
    if (s.socket_created) unlink(s.socket_path.c_str());
    if (s.log >= 0) close(s.log);
    if (s.directory >= 0) close(s.directory);
    s.listener = s.log = s.directory = -1;
    s.socket_created = false;
}
#else
void GuestControl_Poll() {}
void GuestControl_Shutdown() {}
#endif
