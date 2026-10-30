#include "../src/platform/ac1_dinput8_bridge.h"
#include "../src/platform/ac1_pad_discovery.h"
#include "../src/platform/ac1_input_script.h"
#include <cassert>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>
static unsigned char memory[65536];
static uint32_t next=4096;
static constexpr uint32_t base=0x10000000;
static void *resolve(void *,uint32_t p,uint32_t n,uint32_t) {
    return p>=base && uint64_t(p)-base+n<=sizeof(memory)?memory+p-base:nullptr;
}
static uint32_t allocate(void *,uint32_t size,uint32_t alignment) {
    next=(next+alignment-1)&~(alignment-1);uint32_t p=base+next;next+=size;
    assert(next<=sizeof(memory));return p;
}
static void release(void *,uint32_t) {}
static uintptr_t instance(void *,uint32_t) {return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));}
static uint32_t load(uint32_t p){uint32_t n;std::memcpy(&n,resolve(nullptr,p,4,0),4);return n;}
static uint32_t call(uint32_t object,unsigned kind,unsigned slot,std::initializer_list<uint32_t> more) {
    uint32_t a[8]={object},i=1,hr=0xdeadbeef;
    for(auto n:more)a[i++]=n;
    int result=ac1_dinput8_dispatch(AC1_DINPUT8_TOKEN_BASE+kind*0x1000+slot*16,a,i,&hr);
    if(result!=1)std::fprintf(stderr,"slot %u: %s\n",slot,ac1_dinput8_last_error());
    assert(result==1);return hr;
}
int main() {
    _putenv_s("AC1_XINPUT_NATIVE","0");_putenv_s("AC1_INPUT_SCRIPT","");
    ac1_input_session_shutdown();ac1_pad_reset();
    ac1_dinput8_callbacks cb{};cb.struct_size=sizeof(cb);cb.guest_resolve=resolve;
    cb.guest_alloc=allocate;cb.guest_free=release;cb.guest_hinstance_to_host=instance;cb.guest_hwnd_to_host=instance;
    assert(ac1_dinput8_configure(&cb,nullptr));
    const uint32_t iid[]={0xbf798031,0x4da2483a,0x645d99aa,0x009736ed};std::memcpy(memory,iid,16);
    uint32_t hr;assert(ac1_dinput8_create8(1,0x800,base,base+32,0,&hr)==1&&hr==0);
    uint32_t input=load(base+32);assert(input);
    ac1_pad pad{};ac1_pad_submit(0,1,&pad);
    assert(!call(input,3,4,{4,base+48,0,1}));
    ac1_dinput8_enum_batch batch{};assert(ac1_dinput8_enum_batch_take(&batch)==1);
    assert(batch.record_size==0x44c);unsigned matches=0;
    for(unsigned i=0;i<batch.count;i++) {
        const auto *p=static_cast<const unsigned char *>(ac1_dinput8_enum_batch_record(&batch,i));
        if(std::memcmp(p+4,ac1_pad_instance_guid,16))continue;
        ++matches;assert(p[36]==0x15);std::memcpy(memory+64,p+4,16);
        assert(!call(input,3,3,{base+64,base+80,0}));uint32_t device=load(base+80);assert(device);
        uint32_t prop[]={20,16,0,0,0};std::memcpy(memory+96,prop,20);
        assert(!call(device,5,5,{24,base+96}));assert(load(base+112)==0x028e045e);
        assert(call(device,5,2,{})==0);
    }
    assert(matches==1);ac1_dinput8_enum_batch_free(&batch);
    ac1_pad_submit(0,0,nullptr);
    assert(!call(input,3,4,{4,base+48,0,1}));assert(ac1_dinput8_enum_batch_take(&batch)==1);
    for(unsigned i=0;i<batch.count;i++) {
        const auto *p=static_cast<const unsigned char *>(ac1_dinput8_enum_batch_record(&batch,i));
        assert(std::memcmp(p+4,ac1_pad_instance_guid,16));
    }
    ac1_dinput8_enum_batch_free(&batch);call(input,3,2,{});ac1_dinput8_shutdown();
    std::puts("PASS: connected-only Xbox discovery, W record ABI, proxy creation, VID/PID property, release and disconnect enumeration");
}
