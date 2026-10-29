#define WIN32_LEAN_AND_MEAN
#include "ac1_input_script.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <windows.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                              \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)
static ac1_input_script *parse(const char *text) {
    char error[512];
    auto *s = ac1_input_parse(text, error, sizeof(error));
    if (!s)
        std::fprintf(stderr, "%s\n", error);
    CHECK(s);
    return s;
}
int main() {
    CHECK(ac1_input_key_code("DIK_RETURN") == 0x1c);
    CHECK(ac1_input_key_code("enter") == 0x1c);
    CHECK(ac1_input_key_code("DIK_A") == 0x1e);
    CHECK(ac1_input_key_code("0x9c") == 0x9c);
    CHECK(ac1_input_key_code("NOT_A_KEY") == -1);
    struct Mapping {
        unsigned dik, vk, scan;
        bool extended;
    } cases[] = {{0x1c, VK_RETURN, 0x1c, false}, {0x9c, VK_RETURN, 0x1c, true},
                 {0xc8, VK_UP, 0x48, true},      {0x48, VK_NUMPAD8, 0x48, false},
                 {0x1e, 'A', 0x1e, false},       {0x2a, VK_LSHIFT, 0x2a, false},
                 {0x36, VK_RSHIFT, 0x36, false}, {0x9d, VK_RCONTROL, 0x1d, true},
                 {0xb8, VK_RMENU, 0x38, true},   {0x0e, VK_BACK, 0x0e, false},
                 {0xc5, VK_PAUSE, 0x45, false},  {0x45, VK_NUMLOCK, 0x45, true}};
    for (auto c : cases) {
        CHECK(ac1_input_key_vk(c.dik) == c.vk);
        uint32_t lp = 1 | (c.scan << 16) | (c.extended ? 1u << 24 : 0);
        CHECK(ac1_input_key_lparam(c.dik, 1, 0) == lp);
        CHECK(ac1_input_key_lparam(c.dik, 0, 1) == (lp | 0xc0000000));
    }
    auto *bom = parse("\xef\xbb\xbf# UTF-8 with BOM\nat 08 tap RETURN");
    ac1_input_free(bom);
    auto *s =
        parse("# editable timeline\nat 10 tap RETURN 3\n+5 key LSHIFT down\nat +1 key A down\n+1 "
              "key A up\n+1 key LSHIFT up\n+1 mouse move -10 120\n+1 mouse left click\n");
    uint8_t keys[256], mouse[20];
    ac1_input_message m;
    ac1_input_advance(s, 9);
    ac1_input_keyboard(s, keys);
    CHECK(!keys[0x1c]);
    CHECK(!ac1_input_pop(s, &m));
    ac1_input_advance(s, 10);
    ac1_input_keyboard(s, keys);
    CHECK(keys[0x1c] == 0x80);
    CHECK(ac1_input_pop(s, &m));
    CHECK(m.message == WM_KEYDOWN && m.wparam == VK_RETURN && m.lparam == 0x1c0001);
    ac1_input_advance(s, 13);
    ac1_input_keyboard(s, keys);
    CHECK(!keys[0x1c]);
    CHECK(ac1_input_pop(s, &m));
    CHECK(m.message == WM_KEYUP && m.lparam == 0xc01c0001);
    ac1_input_advance(s, 17);
    CHECK(ac1_input_pop(s, &m));
    CHECK(m.wparam == VK_SHIFT && m.keyboard[VK_SHIFT] == 0x80);
    CHECK(ac1_input_pop(s, &m));
    CHECK(m.wparam == 'A' && m.keyboard[VK_SHIFT] == 0x80 && m.keyboard['A'] == 0x80);
    CHECK(ac1_input_pop(s, &m));
    CHECK(m.message == WM_KEYUP && m.keyboard['A'] == 0);
    ac1_input_advance(s, 20);
    while (ac1_input_pop(s, &m)) {
        if (m.message == WM_MOUSEMOVE)
            CHECK(m.x == -10 && m.y == 120 && m.lparam == 0x0078fff6);
    }
    ac1_input_mouse(s, mouse, 20);
    CHECK(mouse[12] == 0x80);
    ac1_input_advance(s, 22);
    ac1_input_mouse(s, mouse, 20);
    CHECK(!mouse[12]);
    ac1_input_free(s);
    s = parse("at 0 dik 0x1e 0x80 poll 1\nat 0 dik 0x1e 0 poll 3\nat 0 mouse state 1 0 0 0 0 0 0 0 "
              "0 0 0 poll 1\nat 0 mouse state 0 0 0 0 0 0 0 0 0 0 0 poll 2");
    ac1_input_advance(s, 0);
    ac1_input_keyboard(s, keys);
    CHECK(keys[0x1e] == 0x80);
    ac1_input_keyboard(s, keys);
    CHECK(keys[0x1e] == 0x80);
    ac1_input_keyboard(s, keys);
    CHECK(!keys[0x1e]);
    ac1_input_mouse(s, mouse, 20);
    CHECK(mouse[0] == 1);
    ac1_input_mouse(s, mouse, 20);
    CHECK(!mouse[0]);
    CHECK(!ac1_input_pop(s, &m));
    ac1_input_free(s);
    s = parse("at 0 dik 0x1e 0x80 poll 2\nat 1 dik 0x1e 0 poll 1");
    ac1_input_advance(s, 0);
    ac1_input_keyboard(s, keys);
    CHECK(!keys[0x1e]);
    ac1_input_keyboard(s, keys);
    CHECK(keys[0x1e] == 0x80);
    ac1_input_advance(s, 1);
    ac1_input_keyboard(s, keys);
    CHECK(!keys[0x1e]);
    ac1_input_free(s);
    const char *bad[] = {"at 0 key WHAT down",
                         "at 0 key A sideways",
                         "at 10 tap A 0",
                         "at 9 tap A\nat 2 tap B",
                         "at 0 mouse move 32768 0",
                         "at 0 mouse state 0 0 0",
                         "at 0 wm 0x10 0 0",
                         "at 18446744073709551615 tap A",
                         "at 1 key A down junk",
                         "at -1 tap A",
                         "at + tap A",
                         "poll 1",
                         "at 0 tap A poll 1",
                         "at ++1 tap A"};
    for (auto text : bad) {
        char error[256];
        s = ac1_input_parse(text, error, sizeof(error));
        CHECK(!s);
        CHECK(std::strstr(error, "line "));
    }
    // Exercise the actual recorder, then feed its bytes back to the parser.
    CreateDirectoryA("work", nullptr);
    const char *path = "work/input_contract_record.txt";
    _putenv_s("AC1_INPUT_SCRIPT", "");
    _putenv_s("AC1_INPUT_RECORD", path);
    char error[512];
    CHECK(ac1_input_session_init(error, sizeof(error)));
    std::memset(keys, 0, 256);
    keys[0x1e] = 0x80;
    ac1_input_record_state(1, keys, 256);
    ac1_input_record_state(1, keys, 256);
    ac1_input_record_message(WM_KEYDOWN, 'A', 0x1e0001);
    int32_t xyz[] = {-9, 4, 120};
    std::memset(mouse, 0, 20);
    std::memcpy(mouse, xyz, 12);
    mouse[13] = 0x80;
    ac1_input_record_state(0, mouse, 20);
    ac1_input_record_message(WM_MOUSEMOVE, 0, 0x00320028);
    ac1_input_session_shutdown();
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    s = parse(text.str().c_str());
    ac1_input_advance(s, 0);
    uint8_t replay[256];
    ac1_input_keyboard(s, replay);
    CHECK(!std::memcmp(keys, replay, 256));
    uint8_t rm[20];
    ac1_input_mouse(s, rm, 20);
    CHECK(!std::memcmp(mouse, rm, 20));
    CHECK(ac1_input_pop(s, &m) && m.message == WM_KEYDOWN);
    CHECK(ac1_input_pop(s, &m) && m.message == WM_MOUSEMOVE && m.x == 40 && m.y == 50);
    CHECK(!ac1_input_pop(s, &m));
    ac1_input_free(s);
    _putenv_s("AC1_INPUT_RECORD", "");
    _putenv_s("AC1_INPUT_SCRIPT", "work/nonexistent-input-script-123.txt");
    CHECK(!ac1_input_session_init(error, sizeof(error)));
    CHECK(std::strstr(error, "cannot open"));
    CHECK(!ac1_input_session_init(error, sizeof(error)));
    ac1_input_session_shutdown();
    _putenv_s("AC1_INPUT_SCRIPT", "");
    std::puts("input script contract passed (parser, timeline, event snapshots, DIK/VK/scan codes, "
              "recorder round trip, fail-closed initialization)");
}
