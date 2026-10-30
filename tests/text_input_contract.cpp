#include "../src/platform/ac1_input_script.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
static uint64_t frame;
extern "C" uint64_t ac1_d3d9_present_count(){return frame;}
static void character(ac1_input_message &m,unsigned cp) {
    assert(m.message==0x100 && m.wparam==0xe7 && m.text_valid && m.text==cp);
    uint16_t value=0;
    const auto *previous=ac1_input_dispatch_state(&m);
    assert(ac1_input_text_translation(0xe7,&value) && value==cp);
    assert(!ac1_input_text_translation('A',&value));
    ac1_input_dispatch_state(previous);
    assert(!ac1_input_text_translation(0xe7,&value));
}
int main() {
    _putenv_s("AC1_INPUT_SCRIPT","");_putenv_s("AC1_INPUT_RECORD","");
    ac1_input_session_shutdown();ac1_input_physical_reset();
    const uint16_t name[]={'A','l','t','a',0xef,'r','4','2'};
    assert(!ac1_input_commit_text(name,8));
    ac1_input_message m{};
    for(auto c:name) {
        assert(ac1_input_next_message(&m));character(m,c);
        assert(ac1_input_next_message(&m));assert(m.message==0x101);
        assert(!ac1_input_next_message(&m)); // never a second unit in the same Present
        frame+=2;
    }
    assert(!ac1_input_next_message(&m));
    const uint16_t invalid[][2]={{'A',0xd800},{'A',0x4e2d},{'A','/'},{'A','_'},{'A',0x20ac},{'A',0xd7}};
    for(auto &text:invalid){assert(ac1_input_commit_text(text,2)==-1);assert(!ac1_input_next_message(&m));}
    uint16_t long_text[257];for(auto &c:long_text)c='A';
    assert(ac1_input_commit_text(long_text,257)==-1);
    for(int i=0;i<4;i++)assert(!ac1_input_commit_text(long_text,128));
    assert(ac1_input_commit_text(name,8)==-2);ac1_input_physical_reset();
    for(unsigned vk:{8u,13u,27u}) {
        assert(!ac1_input_text_key(vk));
        assert(ac1_input_next_message(&m)&&m.message==0x100&&m.wparam==vk&&!m.text_valid);
        uint8_t state[256];unsigned scan=vk==8?0x0e:vk==13?0x1c:1;
        assert(ac1_input_physical_state(1,state,256)==1&&state[scan]==0x80);
        frame+=2;assert(ac1_input_next_message(&m)&&m.message==0x101&&m.wparam==vk);
        assert(ac1_input_physical_state(1,state,256)==1&&!state[scan]);
        frame+=1;
    }
    assert(ac1_input_text_key(46)==-1);
    ac1_input_physical_key_text(0x1e,1,'A'); // physical shifted/layout character
    uint8_t keys[256];assert(ac1_input_physical_state(1,keys,256)==1&&keys[0x1e]==0x80);
    assert(ac1_input_next_message(&m)&&m.wparam=='A'&&m.text=='A'&&m.text_valid);
    ac1_input_physical_key_text(0x1e,0,'A');assert(ac1_input_next_message(&m)&&m.message==0x101);
    ac1_input_physical_key_text(0x1e,1,0x4e2d);
    assert(ac1_input_next_message(&m)&&m.text_valid&&!m.text); // no US fallback substitution
    ac1_input_physical_reset();
    char error[512];auto *s=ac1_input_parse("0 text 0x41 0xef\n4 textkey BACKSPACE\n7 textkey ENTER\n10 textkey ESCAPE\n",error,sizeof(error));assert(s);
    ac1_input_advance(s,0);assert(ac1_input_pop(s,&m));character(m,'A');assert(ac1_input_pop(s,&m));
    ac1_input_advance(s,2);assert(ac1_input_pop(s,&m));character(m,0xef);assert(ac1_input_pop(s,&m));assert(!ac1_input_pop(s,&m));
    ac1_input_advance(s,12);for(unsigned vk:{8u,13u,27u}){assert(ac1_input_pop(s,&m)&&m.wparam==vk);assert(ac1_input_pop(s,&m));}
    ac1_input_free(s);
    s=ac1_input_parse("0 text 0xd800",error,sizeof(error));assert(!s&&std::strstr(error,"line 1"));
    std::puts("PASS: atomic UTF-16/CP1252 repertoire, event snapshots, control keys, physical DI state, no substitution, queue bounds and replay text parser");
}
