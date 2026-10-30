#include "../src/platform/ac1_gamepad.h"
#include "../src/platform/ac1_input_script.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <windows.h>
static uint64_t frame;
extern "C" uint64_t ac1_d3d9_present_count() { return frame; }
int main() {
    _putenv_s("AC1_XINPUT_NATIVE", "0");
    _putenv_s("AC1_INPUT_SCRIPT", "");
    ac1_input_session_shutdown(); ac1_pad_reset();
    ac1_pad_state state{}, before{};
    std::memset(&state,0xa5,sizeof(state)); before=state;
    assert(ac1_pad_get_state(0,&state)==1167);
    assert(!std::memcmp(&before,&state,16));
    assert(ac1_pad_get_state(4,&state)==160);
    assert(ac1_pad_get_state(0,nullptr)==160);
    ac1_pad raw{0xf3ff,1,255,-32768,32767,-1,1};
    ac1_pad_submit(0,1,&raw);
    assert(!ac1_pad_get_state(0,&state));
    assert(!std::memcmp(&state.pad,&raw,12));
    uint32_t packet=state.packet;
    ac1_pad_submit(0,1,&raw); assert(!ac1_pad_get_state(0,&state)); assert(state.packet==packet);
    raw.rt=30; ac1_pad_submit(0,1,&raw);assert(!ac1_pad_get_state(0,&state));assert(state.packet==packet+1&&state.pad.rt==30);
    ac1_pad_caps caps{};assert(!ac1_pad_get_caps(0,1,&caps));
    assert(caps.type==1&&caps.subtype==1&&caps.pad.buttons==0xf3ff&&caps.pad.rt==255&&caps.low_motor==0);
    assert(ac1_pad_get_caps(0,2,&caps)==160);
    uint16_t motors[]={65535,32768};assert(!ac1_pad_set_state(0,motors));
    ac1_pad_submit(0,0,&raw);assert(ac1_pad_set_state(0,motors)==1167);
    ac1_pad_submit(0,1,nullptr);assert(!ac1_pad_get_state(0,&state));assert(state.pad.buttons==0&&state.pad.lx==0);
    ac1_pad_submit(3,1,&raw);assert(!ac1_pad_get_state(3,&state));assert(state.pad.rt==30);
    ac1_pad_reset();assert(ac1_pad_get_state(3,&state)==1167);
    const char *script="at 0 pad connect\nat 2 pad stick left -32768 32767\n"
        "at 2 pad stick right -1 1\nat 3 pad trigger LT 30\nat 3 pad trigger RT 255\n"
        "at 4 pad button A down\nat 5 pad button A down\nat 6 pad button A up\n"
        "at 7 pad release\nat 8 pad disconnect\nat 9 pad connect\n";
    char error[1024];auto *s=ac1_input_parse(script,error,sizeof(error));assert(s);
    ac1_pad p{};int connected;uint32_t n;
    ac1_input_advance(s,0);ac1_input_pad(s,&p,&connected,&n);assert(connected&&n==1);
    ac1_input_advance(s,2);ac1_input_pad(s,&p,&connected,&n);assert(n==3&&p.lx==-32768&&p.ly==32767&&p.rx==-1&&p.ry==1);
    ac1_input_advance(s,4);ac1_input_pad(s,&p,&connected,&n);assert(n==6&&p.buttons==4096&&p.lt==30&&p.rt==255);
    ac1_input_advance(s,5);ac1_input_pad(s,&p,&connected,&n);assert(n==6);
    ac1_input_advance(s,7);ac1_input_pad(s,&p,&connected,&n);assert(n==8&&!p.buttons&&!p.rt&&!p.lx);
    ac1_input_advance(s,8);ac1_input_pad(s,&p,&connected,&n);assert(!connected&&n==9);
    ac1_input_advance(s,9);ac1_input_pad(s,&p,&connected,&n);assert(connected&&n==10);ac1_input_free(s);
    const char *invalid[]={"0 pad stick left 32768 0","0 pad trigger RT 256","0 pad button Q down","0 pad connect extra","0 pad stick bad 0 0","0 pad release poll 1"};
    for(auto text:invalid){s=ac1_input_parse(text,error,sizeof(error));assert(!s&&std::strstr(error,"line 1"));}
    // Actual session/provider integration, including physical isolation and
    // commands driven by successful Present rather than polling frequency.
    std::ofstream("work/gamepad/contract-replay.txt")<<script;
    _putenv_s("AC1_INPUT_SCRIPT","work/gamepad/contract-replay.txt");
    ac1_input_session_shutdown();ac1_pad_submit(0,1,&raw);
    frame=0;assert(!ac1_pad_get_state(0,&state));assert(!state.pad.buttons&&!state.pad.rt);
    frame=4;assert(!ac1_pad_get_state(0,&state));assert(state.pad.buttons==4096&&state.pad.rt==255);
    frame=8;assert(ac1_pad_get_state(0,&state)==1167);
    frame=9;assert(!ac1_pad_get_state(0,&state));assert(!state.pad.buttons);
    assert(ac1_pad_get_state(1,&state)==1167);
    ac1_input_session_shutdown();_putenv_s("AC1_INPUT_SCRIPT","");ac1_pad_reset();
    // Native forwarding oracle, whether or not hardware is attached.
    HMODULE module=LoadLibraryExW(L"xinput1_4.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    assert(module);
    using state_fn=DWORD(WINAPI *)(DWORD,void *);
    using caps_fn=DWORD(WINAPI *)(DWORD,DWORD,void *);
    FARPROC entry=GetProcAddress(module,"XInputGetState");state_fn get;
    std::memcpy(&get,&entry,sizeof(get));assert(get);
    entry=GetProcAddress(module,"XInputGetCapabilities");caps_fn capabilities;
    std::memcpy(&capabilities,&entry,sizeof(capabilities));assert(capabilities);
    entry=GetProcAddress(module,"XInputSetState");state_fn set;
    std::memcpy(&set,&entry,sizeof(set));assert(set);
    _putenv_s("AC1_XINPUT_NATIVE","1");
    for(unsigned user=0;user<4;user++) {
        ac1_pad_state native_state{};ac1_pad_caps native_caps{};
        uint32_t expected=get(user,&native_state);assert(ac1_pad_get_state(user,&state)==expected);
        if(!expected)assert(!std::memcmp(&state,&native_state,sizeof(state)));
        expected=capabilities(user,0,&native_caps);assert(ac1_pad_get_caps(user,0,&caps)==expected);
        if(!expected)assert(!std::memcmp(&caps,&native_caps,sizeof(caps)));
        uint16_t silent[2]={0,0};assert(ac1_pad_set_state(user,silent)==set(user,silent));
    }
    FreeLibrary(module);_putenv_s("AC1_XINPUT_NATIVE","0");
    std::puts("PASS: System32 native XInput state/capabilities/vibration forwarding oracle (four slots)");
    std::puts("PASS: raw XInput ABI, packets, triggers, masks, slots, disconnect, capabilities, replay parser and Present-driven provider isolation");
}
