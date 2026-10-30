/* Real suspended-thread unwind fixture: leaf, nested guest frames and shutdown. */
#include "ac1_profile.h"
#include <windows.h>
#include <stdint.h>
static volatile uint64_t sink;
static uint32_t eip=0x00403010;
static unsigned owner;
__attribute__((noinline)) void L_00403000(void)
{
    ac1_profile_entry(0x00403000);
    ac1_profile_present();
    ULONGLONG end=GetTickCount64()+1200;
    do {
        for(unsigned i=0;i<10000;++i) sink=sink*1664525+1013904223;
    } while(GetTickCount64()<end);
    Sleep(180); /* host wait must retain the same nested guest ancestry */
    sink++; /* retain the guest activation across the host call */
}
__attribute__((noinline)) void L_00402000(unsigned depth)
{
    ac1_profile_entry(0x00402000);
    if(depth) L_00402000(depth-1); else L_00403000();
    sink++;
}
__attribute__((noinline)) void L_00401000(void) { ac1_profile_entry(0x00401000); L_00402000(1); sink++; }
int main(void)
{
    ac1_profile_init();
    ac1_profile_guest(0,1,&eip,&owner);
    L_00401000();
    ac1_profile_guest_done();
    ac1_profile_shutdown();
    return 0;
}
