#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include "ac1_input_script.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#if defined(_WIN32) && !defined(AC1_PORTABLE_TEST)
#include <dinput.h>
#else
#include "ac1_input_codes.h"
#include <chrono>
static uint64_t GetTickCount64() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
#endif
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(_WIN32) && !defined(AC1_PORTABLE_TEST)
#include <windows.h>
#endif
#ifdef AC1_HOST_D3D9
extern "C" uint64_t ac1_d3d9_present_count(void);
#endif
namespace {
struct KeyName {
    const char *name;
    unsigned code;
};
#define KEY(x) {#x, DIK_##x}
const KeyName names[] = {KEY(ESCAPE),
                         KEY(1),
                         KEY(2),
                         KEY(3),
                         KEY(4),
                         KEY(5),
                         KEY(6),
                         KEY(7),
                         KEY(8),
                         KEY(9),
                         KEY(0),
                         KEY(MINUS),
                         KEY(EQUALS),
                         KEY(BACK),
                         KEY(TAB),
                         KEY(Q),
                         KEY(W),
                         KEY(E),
                         KEY(R),
                         KEY(T),
                         KEY(Y),
                         KEY(U),
                         KEY(I),
                         KEY(O),
                         KEY(P),
                         KEY(LBRACKET),
                         KEY(RBRACKET),
                         KEY(RETURN),
                         KEY(LCONTROL),
                         KEY(A),
                         KEY(S),
                         KEY(D),
                         KEY(F),
                         KEY(G),
                         KEY(H),
                         KEY(J),
                         KEY(K),
                         KEY(L),
                         KEY(SEMICOLON),
                         KEY(APOSTROPHE),
                         KEY(GRAVE),
                         KEY(LSHIFT),
                         KEY(BACKSLASH),
                         KEY(Z),
                         KEY(X),
                         KEY(C),
                         KEY(V),
                         KEY(B),
                         KEY(N),
                         KEY(M),
                         KEY(COMMA),
                         KEY(PERIOD),
                         KEY(SLASH),
                         KEY(RSHIFT),
                         KEY(MULTIPLY),
                         KEY(LMENU),
                         KEY(SPACE),
                         KEY(CAPITAL),
                         KEY(F1),
                         KEY(F2),
                         KEY(F3),
                         KEY(F4),
                         KEY(F5),
                         KEY(F6),
                         KEY(F7),
                         KEY(F8),
                         KEY(F9),
                         KEY(F10),
                         KEY(F11),
                         KEY(F12),
                         KEY(F13),
                         KEY(F14),
                         KEY(F15),
                         KEY(NUMLOCK),
                         KEY(SCROLL),
                         KEY(NUMPAD7),
                         KEY(NUMPAD8),
                         KEY(NUMPAD9),
                         KEY(SUBTRACT),
                         KEY(NUMPAD4),
                         KEY(NUMPAD5),
                         KEY(NUMPAD6),
                         KEY(ADD),
                         KEY(NUMPAD1),
                         KEY(NUMPAD2),
                         KEY(NUMPAD3),
                         KEY(NUMPAD0),
                         KEY(DECIMAL),
                         KEY(OEM_102),
                         KEY(NUMPADENTER),
                         KEY(RCONTROL),
                         KEY(DIVIDE),
                         KEY(SYSRQ),
                         KEY(RMENU),
                         KEY(PAUSE),
                         KEY(HOME),
                         KEY(UP),
                         KEY(PRIOR),
                         KEY(LEFT),
                         KEY(RIGHT),
                         KEY(END),
                         KEY(DOWN),
                         KEY(NEXT),
                         KEY(INSERT),
                         KEY(DELETE),
                         KEY(LWIN),
                         KEY(RWIN),
                         KEY(APPS),
                         {"ENTER", DIK_RETURN},
                         {"BACKSPACE", DIK_BACK},
                         {"ESC", DIK_ESCAPE},
                         {"LALT", DIK_LMENU},
                         {"RALT", DIK_RMENU},
                         {"PAGEUP", DIK_PRIOR},
                         {"PAGEDOWN", DIK_NEXT}};
#undef KEY
#if defined(_WIN32) && !defined(AC1_PORTABLE_TEST)
HKL layout() {
    static HKL value = LoadKeyboardLayoutW(L"00000409", KLF_NOTELLSHELL);
    return value;
}
#endif
uint64_t number(const std::string &s, uint64_t max = UINT64_MAX) {
    if (s.empty() || s[0] == '-' || s[0] == '+')
        throw std::runtime_error("expected unsigned integer: " + s);
    size_t end = 0;
    auto v =
        std::stoull(s, &end, s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') ? 16 : 10);
    if (end != s.size() || v > max)
        throw std::runtime_error("integer out of range: " + s);
    return v;
}
int32_t signed_number(const std::string &s) {
    size_t end = 0;
    size_t start = !s.empty() && (s[0] == '-' || s[0] == '+') ? 1 : 0;
    bool hex = s.size() > start + 2 && s[start] == '0' && (s[start+1] == 'x' || s[start+1] == 'X');
    auto v = std::stoll(s, &end, hex ? 16 : 10);
    if (end != s.size() || v < INT32_MIN || v > INT32_MAX)
        throw std::runtime_error("signed integer out of range: " + s);
    return static_cast<int32_t>(v);
}
enum Type { Key, Dik, Move, Button, MouseState, Wm, PadConnect, PadButton, PadStick, PadTrigger, PadRelease, Text };
struct Event {
    uint64_t frame;
    Type type;
    uint32_t a = 0, b = 0, c = 0;
    std::array<uint8_t, 20> mouse{};
    uint64_t poll = 0;
};
void error_out(char *out, size_t size, const std::string &s) {
    if (out && size)
        std::snprintf(out, size, "%s", s.c_str());
}
} // namespace
struct ac1_input_script {
    std::vector<Event> events;
    size_t next = 0;
    uint64_t frame = 0, keyboard_poll = 0, mouse_poll = 0;
    uint64_t keyboard_poll_frame = UINT64_MAX, mouse_poll_frame = UINT64_MAX;
    size_t next_keyboard_sample = 0, next_mouse_sample = 0;
    std::vector<Event> keyboard_samples, mouse_samples;
    std::array<uint8_t, 256> keys{}, vk{};
    std::array<uint8_t, 20> mouse{};
    ac1_pad pad{};
    int pad_connected = 0;
    uint32_t pad_packet = 0;
    int32_t x = 0, y = 0;
    std::deque<ac1_input_message> messages;
};
extern "C" int ac1_input_key_code(const char *name) {
    if (!name)
        return -1;
    std::string s(name);
    for (char &c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (s.compare(0, 4, "DIK_") == 0)
        s.erase(0, 4);
    for (const auto &n : names)
        if (s == n.name)
            return static_cast<int>(n.code);
    try {
        if (s.size() > 2 && s.substr(0, 2) == "0X")
            return static_cast<int>(number(s, 255));
    } catch (...) {
    }
    return -1;
}
extern "C" uint32_t ac1_input_key_vk(unsigned dik) {
    if (dik > 255)
        return 0;
    switch (dik) {
    case DIK_NUMPAD0:
        return VK_NUMPAD0;
    case DIK_NUMPAD1:
        return VK_NUMPAD1;
    case DIK_NUMPAD2:
        return VK_NUMPAD2;
    case DIK_NUMPAD3:
        return VK_NUMPAD3;
    case DIK_NUMPAD4:
        return VK_NUMPAD4;
    case DIK_NUMPAD5:
        return VK_NUMPAD5;
    case DIK_NUMPAD6:
        return VK_NUMPAD6;
    case DIK_NUMPAD7:
        return VK_NUMPAD7;
    case DIK_NUMPAD8:
        return VK_NUMPAD8;
    case DIK_NUMPAD9:
        return VK_NUMPAD9;
    case DIK_DECIMAL:
        return VK_DECIMAL;
    default:
        break;
    }
    if (dik == DIK_PAUSE)
        return VK_PAUSE;
    if (dik == DIK_SYSRQ)
        return VK_SNAPSHOT;
    if (dik == DIK_NUMLOCK)
        return VK_NUMLOCK;
    #if defined(_WIN32) && !defined(AC1_PORTABLE_TEST)
    return MapVirtualKeyExW((dik & 0x7f) | (dik & 0x80 ? 0xe000 : 0), MAPVK_VSC_TO_VK_EX, layout());
    #else
    static const uint8_t vk[256] = {0,27,49,50,51,52,53,54,55,56,57,48,189,187,8,9,81,87,69,82,84,89,85,73,79,80,219,221,13,162,65,83,68,70,71,72,74,75,76,186,222,192,160,220,90,88,67,86,66,78,77,188,190,191,161,0,164,32,20,112,113,114,115,116,117,118,119,120,121,144,145,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,122,123,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,13,163,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,111,0,44,165,0,0,0,0,0,0,0,0,0,0,0,0,19,0,36,38,33,0,37,0,39,0,35,40,34,45,46,0,0,0,0,0,0,0,91,92,93,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    return vk[dik];
    #endif
}
extern "C" uint32_t ac1_input_key_lparam(unsigned dik, int down, int previous) {
    if (dik > 255)
        return 0;
    // Pause uses E1; NumLock carries the extended flag in a window key message.
    unsigned scan = dik == DIK_PAUSE ? 0x45 : dik & 0x7f;
    bool extended = ((dik & 0x80) && dik != DIK_PAUSE) || dik == DIK_NUMLOCK;
    return 1u | (scan << 16) | (extended ? 1u << 24 : 0) | ((previous || !down) ? 1u << 30 : 0) |
           (!down ? 1u << 31 : 0);
}
extern "C" ac1_input_script *ac1_input_parse(const char *text, char *error, size_t capacity) {
    auto *s = new ac1_input_script;
    unsigned line_no = 0;
    try {
        std::istringstream input(text ? text : "");
        std::string line;
        uint64_t previous = 0;
        while (std::getline(input, line)) {
            ++line_no;
            if (line_no == 1 && line.compare(0, 3, "\xef\xbb\xbf") == 0)
                line.erase(0, 3);
            line.erase(line.find('#') == std::string::npos ? line.size() : line.find('#'));
            std::istringstream words(line);
            std::vector<std::string> t;
            std::string word;
            while (words >> word)
                t.push_back(word);
            if (t.empty())
                continue;
            uint64_t poll = 0;
            if (t.size() >= 2 && t[t.size() - 2] == "poll") {
                poll = number(t.back());
                if (!poll)
                    throw std::runtime_error("poll ordinal starts at one");
                t.resize(t.size() - 2);
            }
            if (t.empty())
                throw std::runtime_error("missing command before poll ordinal");
            size_t first_event = s->events.size();
            size_t pos = 0;
            if (t[pos] == "at")
                ++pos;
            if (pos == t.size())
                throw std::runtime_error("missing present index");
            std::string time = t[pos++];
            bool relative = time[0] == '+';
            uint64_t frame = number(relative ? time.substr(1) : time);
            if (relative) {
                if (frame > UINT64_MAX - previous)
                    throw std::runtime_error("present overflow");
                frame += previous;
            }
            if (frame < previous)
                throw std::runtime_error("command presents must be nondecreasing");
            previous = frame;
            auto need = [&](size_t n) {
                if (t.size() - pos != n)
                    throw std::runtime_error("wrong argument count");
            };
            auto state = [](const std::string &v) -> uint32_t {
                if (v == "down")
                    return 0x80;
                if (v == "up")
                    return 0;
                throw std::runtime_error("expected down or up");
            };
            if (pos == t.size())
                throw std::runtime_error("missing command");
            std::string command = t[pos++];
            if (command == "key" || command == "tap" || command == "dik") {
                if (command == "tap") {
                    if (t.size() - pos < 1 || t.size() - pos > 2)
                        throw std::runtime_error("tap KEY [frames]");
                } else
                    need(2);
                int k = ac1_input_key_code(t[pos++].c_str());
                if (k < 0)
                    throw std::runtime_error("unknown DIK name");
                if (command != "dik" && !ac1_input_key_vk(k))
                    throw std::runtime_error("DIK has no VK mapping; use raw dik command");
                uint32_t value = command == "tap"   ? 0x80
                                 : command == "dik" ? static_cast<uint32_t>(number(t[pos++], 255))
                                                    : state(t[pos++]);
                s->events.push_back(
                    {frame, command == "dik" ? Dik : Key, static_cast<uint32_t>(k), value});
                if (command == "tap") {
                    uint64_t duration = pos < t.size() ? number(t[pos], UINT32_MAX) : 2;
                    if (!duration || frame > UINT64_MAX - duration)
                        throw std::runtime_error("invalid tap duration");
                    s->events.push_back({frame + duration, Key, static_cast<uint32_t>(k), 0});
                }
            } else if (command == "text") {
                if (pos == t.size()) throw std::runtime_error("text requires UTF-16 code units (e.g. 0x41 0x62)");
                size_t count=t.size()-pos;
                if(count>256 || frame>UINT64_MAX-count*2)throw std::runtime_error("text exceeds 256 units or Present range");
                uint64_t text_frame=frame;
                while (pos < t.size()) {
                    unsigned unit = unsigned(number(t[pos++],65535));
                    if (!ac1_input_text_supported(unit)) throw std::runtime_error("text unit is outside AC1's CP1252 name repertoire");
                    s->events.push_back({text_frame,Text,unit});text_frame+=2;
                }
            } else if (command == "textkey") {
                need(1);
                unsigned vk = t[pos] == "BACKSPACE" ? 8 : t[pos] == "ENTER" ? 13 : t[pos] == "ESCAPE" ? 27 : 0;
                if (!vk) throw std::runtime_error("textkey expects BACKSPACE, ENTER or ESCAPE");
                if(frame>UINT64_MAX-2)throw std::runtime_error("textkey Present overflow");
                unsigned scan=vk==8?DIK_BACK:vk==13?DIK_RETURN:DIK_ESCAPE;
                s->events.push_back({frame,Key,scan,0x80});
                s->events.push_back({frame+2,Key,scan,0});
            } else if (command == "pad") {
                if (pos == t.size()) throw std::runtime_error("missing pad command");
                std::string sub = t[pos++];
                if (sub == "connect" || sub == "disconnect" || sub == "release") {
                    need(0);
                    s->events.push_back({frame, sub == "release" ? PadRelease : PadConnect,
                                         uint32_t(sub == "connect")});
                } else if (sub == "button") {
                    need(2);
                    static const char *buttons[] = {"UP","DOWN","LEFT","RIGHT","START","BACK",
                        "LS","RS","LB","RB",nullptr,nullptr,"A","B","X","Y"};
                    unsigned bit = 16;
                    for (unsigned i=0;i<16;++i) if (buttons[i] && t[pos] == buttons[i]) bit=i;
                    if (bit == 16) throw std::runtime_error("unknown pad button: " + t[pos]);
                    s->events.push_back({frame, PadButton, 1u << bit, state(t[pos+1])});
                } else if (sub == "stick") {
                    need(3);
                    if (t[pos] != "left" && t[pos] != "right")
                        throw std::runtime_error("pad stick left|right X Y");
                    int x=signed_number(t[pos+1]), y=signed_number(t[pos+2]);
                    if (x < -32768 || x > 32767 || y < -32768 || y > 32767)
                        throw std::runtime_error("pad stick requires int16 raw values");
                    s->events.push_back({frame, PadStick, uint32_t(t[pos] == "right"), uint32_t(x), uint32_t(y)});
                } else if (sub == "trigger") {
                    need(2);
                    if (t[pos] != "LT" && t[pos] != "RT")
                        throw std::runtime_error("pad trigger LT|RT 0..255");
                    s->events.push_back({frame, PadTrigger, uint32_t(t[pos] == "RT"),
                                         uint32_t(number(t[pos+1],255))});
                } else throw std::runtime_error("unknown pad command: " + sub);
            } else if (command == "mouse") {
                if (pos == t.size())
                    throw std::runtime_error("missing mouse command");
                std::string sub = t[pos++];
                if (sub == "move") {
                    need(2);
                    int32_t x = signed_number(t[pos]), y = signed_number(t[pos + 1]);
                    if (x < INT16_MIN || x > INT16_MAX || y < INT16_MIN || y > INT16_MAX)
                        throw std::runtime_error("client coordinate exceeds WM_MOUSEMOVE range");
                    s->events.push_back(
                        {frame, Move, static_cast<uint32_t>(x), static_cast<uint32_t>(y)});
                } else if (sub == "state") {
                    need(11);
                    Event e{frame, MouseState};
                    for (int i = 0; i < 3; ++i) {
                        int32_t v = signed_number(t[pos++]);
                        std::memcpy(e.mouse.data() + i * 4, &v, 4);
                    }
                    for (int i = 0; i < 8; ++i)
                        e.mouse[12 + i] = static_cast<uint8_t>(number(t[pos++], 255));
                    s->events.push_back(e);
                } else {
                    need(1);
                    unsigned button = sub == "left"     ? 0
                                      : sub == "right"  ? 1
                                      : sub == "middle" ? 2
                                                        : static_cast<unsigned>(number(sub, 7));
                    std::string action = t[pos];
                    s->events.push_back(
                        {frame, Button, button, action == "click" ? 0x80 : state(action)});
                    if (action == "click") {
                        if (frame > UINT64_MAX - 2)
                            throw std::runtime_error("click overflow");
                        s->events.push_back({frame + 2, Button, button, 0});
                    }
                }
            } else if (command == "wm") {
                need(3);
                uint32_t m = static_cast<uint32_t>(number(t[pos], UINT32_MAX));
                if (m != WM_KEYDOWN && m != WM_KEYUP && m != WM_MOUSEMOVE)
                    throw std::runtime_error("wm supports KEYDOWN/KEYUP/MOUSEMOVE only");
                s->events.push_back({frame, Wm, m,
                                     static_cast<uint32_t>(number(t[pos + 1], UINT32_MAX)),
                                     static_cast<uint32_t>(number(t[pos + 2], UINT32_MAX))});
            } else
                throw std::runtime_error("unknown command: " + command);
            if (poll) {
                Event e = s->events.back();
                if (s->events.size() != first_event + 1 || (e.type != Dik && e.type != MouseState))
                    throw std::runtime_error("poll is only valid on raw dik/mouse state commands");
                e.poll = poll;
                auto &samples = e.type == Dik ? s->keyboard_samples : s->mouse_samples;
                if (!samples.empty() && frame == samples.back().frame && poll < samples.back().poll)
                    throw std::runtime_error("poll ordinals must be nondecreasing per device");
                samples.push_back(e);
                s->events.pop_back();
            }
        }
        std::stable_sort(s->events.begin(), s->events.end(),
                         [](const Event &a, const Event &b) { return a.frame < b.frame; });
        error_out(error, capacity, "");
        return s;
    } catch (const std::exception &e) {
        error_out(error, capacity,
                  "input script line " + std::to_string(line_no) + ": " + e.what());
        delete s;
        return nullptr;
    }
}
extern "C" void ac1_input_free(ac1_input_script *s) { delete s; }
namespace {
void vk_change(ac1_input_script *s, unsigned vk, bool down) {
    if (vk > 255)
        return;
    if (down && !(s->vk[vk] & 0x80) && (vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL))
        s->vk[vk] ^= 1;
    s->vk[vk] = (s->vk[vk] & 1) | (down ? 0x80 : 0);
    s->vk[VK_SHIFT] = (s->vk[VK_LSHIFT] | s->vk[VK_RSHIFT]) & 0x80;
    s->vk[VK_CONTROL] = (s->vk[VK_LCONTROL] | s->vk[VK_RCONTROL]) & 0x80;
    s->vk[VK_MENU] = (s->vk[VK_LMENU] | s->vk[VK_RMENU]) & 0x80;
}
void message(ac1_input_script *s, uint32_t m, uint32_t wp, uint32_t lp) {
    ac1_input_message v{};
    v.message = m;
    v.wparam = wp;
    v.lparam = lp;
    v.x = s->x;
    v.y = s->y;
    std::memcpy(v.keyboard, s->vk.data(), 256);
    s->messages.push_back(v);
}
} // namespace
extern "C" int ac1_input_text_supported(uint32_t c) {
    // Matches the portable GetStringTypeW C1_ALPHA/DIGIT subset and the
    // explicit space/plus/minus exceptions at 0077ffd7. Every accepted unit
    // maps identically to a CP1252 byte; no best-fit or '?' substitution.
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == ' ' || c == '+' || c == '-' ||
        (c >= 0xc0 && c <= 0xff && c != 0xd7 && c != 0xf7);
}
namespace {
void text_event(ac1_input_script *s, unsigned vk, uint16_t unit, bool supplied) {
    message(s,WM_KEYDOWN,vk,1);
    s->messages.back().text = unit; s->messages.back().text_valid = supplied;
    message(s,WM_KEYUP,vk,0xc0000001u);
    s->messages.back().text = unit; s->messages.back().text_valid = supplied;
}
}
extern "C" void ac1_input_advance(ac1_input_script *s, uint64_t frame) {
    if (!s)
        return;
    s->frame = frame;
    while (s->next < s->events.size() && s->events[s->next].frame <= frame) {
        const Event &e = s->events[s->next++];
        ac1_pad old_pad = s->pad;
        int old_connected = s->pad_connected;
        switch (e.type) {
        case Text: text_event(s,0xe7,uint16_t(e.a),true); break; // VK_PACKET -> ToUnicode
        case PadConnect:
            s->pad_connected = e.a;
            s->pad = {};
            break;
        case PadRelease: s->pad = {}; break;
        case PadButton:
            s->pad.buttons = e.b ? s->pad.buttons | e.a : s->pad.buttons & ~e.a;
            break;
        case PadStick:
            if (e.a) { s->pad.rx = int16_t(e.b); s->pad.ry = int16_t(e.c); }
            else { s->pad.lx = int16_t(e.b); s->pad.ly = int16_t(e.c); }
            break;
        case PadTrigger:
            if (e.a) s->pad.rt = uint8_t(e.b); else s->pad.lt = uint8_t(e.b);
            break;
        case Key: {
            bool was = (s->keys[e.a] & 0x80) != 0;
            s->keys[e.a] = static_cast<uint8_t>(e.b);
            unsigned vk = ac1_input_key_vk(e.a);
            vk_change(s, vk, e.b != 0);
            unsigned wp = vk;
            if (vk == VK_LSHIFT || vk == VK_RSHIFT)
                wp = VK_SHIFT;
            if (vk == VK_LCONTROL || vk == VK_RCONTROL)
                wp = VK_CONTROL;
            if (vk == VK_LMENU || vk == VK_RMENU)
                wp = VK_MENU;
            message(s, e.b ? WM_KEYDOWN : WM_KEYUP, wp, ac1_input_key_lparam(e.a, e.b != 0, was));
            break;
        }
        case Dik:
            s->keys[e.a] = static_cast<uint8_t>(e.b);
            break;
        case Move:
            s->x = static_cast<int32_t>(e.a);
            s->y = static_cast<int32_t>(e.b);
            message(s, WM_MOUSEMOVE, 0, (e.a & 0xffff) | (e.b << 16));
            break;
        case Button:
            s->mouse[12 + e.a] = static_cast<uint8_t>(e.b);
            break;
        case MouseState:
            s->mouse = e.mouse;
            break;
        case Wm:
            if (e.a == WM_MOUSEMOVE) {
                s->x = static_cast<int16_t>(e.c);
                s->y = static_cast<int16_t>(e.c >> 16);
            } else {
                unsigned vk = e.b;
                unsigned scan = (e.c >> 16) & 255;
                if (vk == VK_SHIFT)
                    vk = scan == 0x36 ? VK_RSHIFT : VK_LSHIFT;
                if (vk == VK_CONTROL)
                    vk = (e.c & (1u << 24)) ? VK_RCONTROL : VK_LCONTROL;
                if (vk == VK_MENU)
                    vk = (e.c & (1u << 24)) ? VK_RMENU : VK_LMENU;
                vk_change(s, vk, e.a == WM_KEYDOWN);
            }
            message(s, e.a, e.b, e.c);
            break;
        }
        if (old_connected != s->pad_connected || std::memcmp(&old_pad, &s->pad, sizeof(old_pad)))
            ++s->pad_packet;
    }
}
extern "C" int ac1_input_pop(ac1_input_script *s, ac1_input_message *m) {
    if (!s || s->messages.empty())
        return 0;
    *m = s->messages.front();
    s->messages.pop_front();
    return 1;
}
extern "C" void ac1_input_keyboard(ac1_input_script *s, uint8_t *state) {
    if (s->keyboard_poll_frame != s->frame) {
        s->keyboard_poll_frame = s->frame;
        s->keyboard_poll = 0;
    }
    ++s->keyboard_poll;
    while (s->next_keyboard_sample < s->keyboard_samples.size()) {
        const Event &e = s->keyboard_samples[s->next_keyboard_sample];
        if (e.frame > s->frame || (e.frame == s->frame && e.poll > s->keyboard_poll))
            break;
        s->keys[e.a] = static_cast<uint8_t>(e.b);
        ++s->next_keyboard_sample;
    }
    std::memcpy(state, s->keys.data(), 256);
}
extern "C" void ac1_input_mouse(ac1_input_script *s, void *state, size_t size) {
    if (s->mouse_poll_frame != s->frame) {
        s->mouse_poll_frame = s->frame;
        s->mouse_poll = 0;
    }
    ++s->mouse_poll;
    while (s->next_mouse_sample < s->mouse_samples.size()) {
        const Event &e = s->mouse_samples[s->next_mouse_sample];
        if (e.frame > s->frame || (e.frame == s->frame && e.poll > s->mouse_poll))
            break;
        s->mouse = e.mouse;
        ++s->next_mouse_sample;
    }
    if (size == 16 || size == 20)
        std::memcpy(state, s->mouse.data(), size);
}
namespace {
std::mutex session_mutex;
bool initialized = false, failed = false;
std::string failure;
ac1_input_script *session = nullptr;
ac1_input_script physical;
uint64_t physical_text_next = 0;
FILE *record = nullptr;
uint64_t started = 0, last_frame = UINT64_MAX, recorded_keyboard_poll = 0, recorded_mouse_poll = 0;
uint64_t recorded_keyboard_frame = UINT64_MAX, recorded_mouse_frame = UINT64_MAX;
std::array<uint8_t, 256> recorded_keys{};
std::array<uint8_t, 20> recorded_mouse{};
thread_local const ac1_input_message *dispatch = nullptr;
uint64_t present() {
#ifdef AC1_HOST_D3D9
    return ac1_d3d9_present_count();
#else
    return 0;
#endif
}
void stamp(const std::string &command) {
    if (record) {
        std::fprintf(record, "at %llu %s # ms=%llu\n", static_cast<unsigned long long>(present()),
                     command.c_str(), static_cast<unsigned long long>(GetTickCount64() - started));
        std::fflush(record);
    }
}
void tick() {
    if (!session) {
        ac1_input_advance(&physical,present());
        if(physical.next==physical.events.size()){physical.events.clear();physical.next=0;}
        return;
    }
    uint64_t f = present();
    size_t before = session->next;
    ac1_input_advance(session, f);
    if (session->next != before)
        std::fprintf(stderr, "AC1 input: present=%llu ms=%llu commands=%zu/%zu\n",
                     static_cast<unsigned long long>(f),
                     static_cast<unsigned long long>(GetTickCount64() - started), session->next,
                     session->events.size());
    const bool clock_changed = f != last_frame && (f == 1 || !(f % 25));
    if (clock_changed) {
        last_frame = f;
        std::fprintf(stderr, "AC1 input clock: present=%llu ms=%llu\n",
                     static_cast<unsigned long long>(f),
                     static_cast<unsigned long long>(GetTickCount64() - started));
    }
    // Preserve clock/command evidence even when the launcher terminates a long run.
    if (session->next != before || clock_changed)
        std::fflush(stderr);
}
} // namespace
extern "C" int ac1_input_session_init(char *error, size_t cap) {
    std::lock_guard<std::mutex> lock(session_mutex);
    if (initialized) {
        error_out(error, cap, failure);
        return !failed;
    }
    initialized = true;
    started = GetTickCount64();
    // Include loader/CRT time before the first USER32/DirectInput call.
    #if defined(_WIN32) && !defined(AC1_PORTABLE_TEST)
    FILETIME creation, exit, kernel, user, now;
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER created_at, current;
        created_at.LowPart = creation.dwLowDateTime;
        created_at.HighPart = creation.dwHighDateTime;
        current.LowPart = now.dwLowDateTime;
        current.HighPart = now.dwHighDateTime;
        if (current.QuadPart >= created_at.QuadPart)
            started -= (current.QuadPart - created_at.QuadPart) / 10000;
    }
    #endif
    const char *script = std::getenv("AC1_INPUT_SCRIPT"), *out = std::getenv("AC1_INPUT_RECORD");
    if (script && *script) {
        std::ifstream file(script, std::ios::binary);
        if (!file)
            failure = "cannot open AC1_INPUT_SCRIPT: " + std::string(script);
        else {
            std::ostringstream text;
            text << file.rdbuf();
            char detail[1024];
            session = ac1_input_parse(text.str().c_str(), detail, sizeof(detail));
            if (!session)
                failure = std::string(script) + ": " + detail;
        }
        if (session)
            std::fprintf(stderr,
                         "AC1 input: replay %s (%zu events); physical keyboard/mouse isolated\n",
                         script, session->events.size());
    }
    if (failure.empty() && out && *out) {
        if (script && *script && std::string(script) == out)
            failure = "AC1_INPUT_RECORD must differ from AC1_INPUT_SCRIPT";
        else {
            record = std::fopen(out, "wb");
            if (!record)
                failure = "cannot open AC1_INPUT_RECORD: " + std::string(out);
            else {
                std::fprintf(record, "# AC1 input v1; present indices, elapsed milliseconds in "
                                     "comments.\n# dik/mouse state are raw DI changes; wm records "
                                     "are delivered independently.\n");
                std::fflush(record);
            }
        }
    }
    failed = !failure.empty();
    error_out(error, cap, failure);
    return !failed;
}
extern "C" void ac1_input_session_shutdown(void) {
    std::lock_guard<std::mutex> lock(session_mutex);
    delete session;
    session = nullptr;
    if (record)
        std::fclose(record);
    record = nullptr;
    initialized = failed = false;
    failure.clear();
    recorded_keys.fill(0);
    recorded_mouse.fill(0);
    last_frame = UINT64_MAX;
    recorded_keyboard_poll = recorded_mouse_poll = 0;
    recorded_keyboard_frame = recorded_mouse_frame = UINT64_MAX;
    dispatch = nullptr;
}
extern "C" int ac1_input_replaying(void) { return session != nullptr; }
extern "C" void ac1_input_tick(void) {
    std::lock_guard<std::mutex> lock(session_mutex);
    tick();
}
extern "C" int ac1_input_replace_state(int keyboard, void *state, size_t size) {
    std::lock_guard<std::mutex> lock(session_mutex);
    if (!session)
        return 0;
    if ((keyboard && size != 256) || (!keyboard && size != 16 && size != 20))
        return -1;
    tick();
    if (keyboard)
        ac1_input_keyboard(session, static_cast<uint8_t *>(state));
    else
        ac1_input_mouse(session, state, size);
    return 1;
}
extern "C" void ac1_input_record_state(int keyboard, const void *state, size_t size) {
    std::lock_guard<std::mutex> lock(session_mutex);
    if (!record)
        return;
    const auto *p = static_cast<const uint8_t *>(state);
    uint64_t &previous_frame = keyboard ? recorded_keyboard_frame : recorded_mouse_frame;
    uint64_t &ordinal = keyboard ? recorded_keyboard_poll : recorded_mouse_poll;
    const uint64_t frame = present();
    if (frame != previous_frame) {
        previous_frame = frame;
        ordinal = 0;
    }
    const uint64_t poll = ++ordinal;
    if (keyboard && size == 256) {
        for (unsigned i = 0; i < 256; ++i)
            if (p[i] != recorded_keys[i]) {
                char command[100];
                std::snprintf(command, sizeof(command), "dik 0x%02x 0x%02x poll %llu", i, p[i],
                              static_cast<unsigned long long>(poll));
                stamp(command);
            }
        std::memcpy(recorded_keys.data(), p, 256);
    } else if (!keyboard && (size == 16 || size == 20) &&
               std::memcmp(recorded_mouse.data(), p, size)) {
        std::memcpy(recorded_mouse.data(), p, size);
        int32_t xyz[3];
        std::memcpy(xyz, p, 12);
        std::ostringstream command;
        command << "mouse state " << xyz[0] << ' ' << xyz[1] << ' ' << xyz[2];
        for (int i = 0; i < 8; ++i)
            command << ' ' << unsigned(recorded_mouse[12 + i]);
        command << " poll " << poll;
        stamp(command.str());
    }
}
extern "C" void ac1_input_record_message(uint32_t m, uint32_t wp, uint32_t lp) {
    if (m != WM_KEYDOWN && m != WM_KEYUP && m != WM_MOUSEMOVE)
        return;
    std::lock_guard<std::mutex> lock(session_mutex);
    char command[100];
    std::snprintf(command, sizeof(command), "wm 0x%x 0x%x 0x%x", m, wp, lp);
    stamp(command);
}
extern "C" int ac1_input_next_message(ac1_input_message *m) {
    std::lock_guard<std::mutex> lock(session_mutex);
    tick();
    return ac1_input_pop(session ? session : &physical, m);
}
extern "C" void ac1_input_cursor(int32_t *x, int32_t *y) {
    std::lock_guard<std::mutex> lock(session_mutex);
    const auto *s = session ? session : &physical;
    *x = dispatch ? dispatch->x : s->x;
    *y = dispatch ? dispatch->y : s->y;
}
extern "C" void ac1_input_vk_state(uint8_t *state) {
    std::lock_guard<std::mutex> lock(session_mutex);
    std::memcpy(state, dispatch ? dispatch->keyboard : (session ? session : &physical)->vk.data(), 256);
}
namespace {
void physical_key(unsigned dik, int down, bool supplied, uint32_t codepoint) {
    std::lock_guard<std::mutex> lock(session_mutex);
    if (session || dik > 255) return;
    bool was = physical.keys[dik] != 0;
    if (was == !!down && !supplied) return;
    physical.keys[dik] = down ? 0x80 : 0;
    unsigned vk = ac1_input_key_vk(dik);
    vk_change(&physical, vk, down != 0);
    unsigned wp = vk;
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) wp = VK_SHIFT;
    if (vk == VK_LCONTROL || vk == VK_RCONTROL) wp = VK_CONTROL;
    if (vk == VK_LMENU || vk == VK_RMENU) wp = VK_MENU;
    if (physical.messages.size() < 1024) {
        message(&physical, down ? WM_KEYDOWN : WM_KEYUP, wp,
                ac1_input_key_lparam(dik, down, was));
        physical.messages.back().text_valid = supplied;
        physical.messages.back().text = ac1_input_text_supported(codepoint) ? uint16_t(codepoint) : 0;
    }
}
}
extern "C" void ac1_input_physical_key(unsigned dik, int down) { physical_key(dik,down,false,0); }
extern "C" void ac1_input_physical_key_text(unsigned dik, int down, uint32_t cp) { physical_key(dik,down,true,cp); }
extern "C" int ac1_input_commit_text(const uint16_t *text, size_t units) {
    std::lock_guard<std::mutex> lock(session_mutex);
    if(session)return -3;
    if((!text && units) || units > 256)return -1;
    for(size_t i=0;i<units;++i)if(!ac1_input_text_supported(text[i]))return -1;
    if(physical.messages.size()+(physical.events.size()-physical.next)*2+units*2>1024)return -2;
    uint64_t frame=std::max(present(),physical_text_next);
    for(size_t i=0;i<units;++i)physical.events.push_back({frame+i*2,Text,text[i]});
    physical_text_next=frame+units*2;
    return 0;
}
extern "C" int ac1_input_text_key(unsigned vk) {
    std::lock_guard<std::mutex> lock(session_mutex);
    if(session)return -3;
    if(vk!=8 && vk!=13 && vk!=27)return -1;
    if(physical.messages.size()+(physical.events.size()-physical.next)*2+2>1024)return -2;
    uint64_t frame=std::max(present(),physical_text_next);
    unsigned scan=vk==8?DIK_BACK:vk==13?DIK_RETURN:DIK_ESCAPE;
    physical.events.push_back({frame,Key,scan,0x80});
    physical.events.push_back({frame+2,Key,scan,0});
    physical_text_next=frame+3;return 0;
}
extern "C" int ac1_input_text_translation(unsigned vk, uint16_t *text) {
    if(!dispatch || !dispatch->text_valid || dispatch->wparam!=vk ||
       (dispatch->message!=WM_KEYDOWN && dispatch->message!=WM_KEYUP))return 0;
    *text=dispatch->text;
    if(std::getenv("AC1_TEXT_TRACE"))
        std::fprintf(stderr,"AC1 text ToUnicode: message=%04x vk=%02x UTF16=%04x CP1252=%02x\n",
                     dispatch->message,vk,*text,*text);
    return 1;
}
extern "C" void ac1_input_physical_mouse(int32_t dx, int32_t dy, int32_t wheel, unsigned buttons) {
    std::lock_guard<std::mutex> lock(session_mutex);
    if (session) return;
    int32_t delta[3]; std::memcpy(delta, physical.mouse.data(), 12);
    int32_t add[3] = {dx,dy,wheel};
    for(unsigned i=0;i<3;++i) delta[i]=static_cast<int32_t>(
        std::max<int64_t>(INT32_MIN,std::min<int64_t>(INT32_MAX,int64_t(delta[i])+add[i])));
    std::memcpy(physical.mouse.data(),delta,12);
    for(unsigned i=0;i<8;++i) physical.mouse[12+i] = buttons & (1u<<i) ? 0x80 : 0;
    physical.x = static_cast<int32_t>(uint32_t(physical.x)+uint32_t(dx));
    physical.y = static_cast<int32_t>(uint32_t(physical.y)+uint32_t(dy));
    if((dx||dy) && physical.messages.size()<1024)
        message(&physical,WM_MOUSEMOVE,0,(uint32_t(physical.x)&0xffff)|(uint32_t(physical.y)<<16));
}
extern "C" int ac1_input_physical_state(int keyboard,void *state,size_t size) {
    std::lock_guard<std::mutex> lock(session_mutex);
    if(!state || (keyboard ? size!=256 : size!=16 && size!=20))return -1;
    tick();
    std::memcpy(state,keyboard?physical.keys.data():physical.mouse.data(),size);
    if(!keyboard)std::memset(physical.mouse.data(),0,12);
    return 1;
}
extern "C" void ac1_input_physical_reset(void) {
    std::lock_guard<std::mutex> lock(session_mutex);
    physical = ac1_input_script{};physical_text_next=0;
}
extern "C" const ac1_input_message *ac1_input_dispatch_state(const ac1_input_message *m) {
    const ac1_input_message *previous = dispatch;
    dispatch = m;
    return previous;
}

extern "C" void ac1_input_pad(ac1_input_script *s, ac1_pad *out, int *connected, uint32_t *packet) {
    *out = s->pad; *connected = s->pad_connected; *packet = s->pad_packet;
}
extern "C" int ac1_input_session_pad(ac1_pad *out, int *connected, uint32_t *packet) {
    char error[1024];
    if (!ac1_input_session_init(error, sizeof(error))) {
        std::fprintf(stderr, "AC1 XInput: %s\n", error); std::abort();
    }
    std::lock_guard<std::mutex> lock(session_mutex);
    if (!session) return 0;
    tick();
    ac1_input_pad(session, out, connected, packet);
    return 1;
}
