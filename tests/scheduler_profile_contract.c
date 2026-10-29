#include "ac1_profile.h"
#include <windows.h>
#include <stdio.h>

static HANDLE stop;
static uint32_t eips[2]={0x1000,0x2000};
static unsigned owner=1;
static DWORD WINAPI worker(void *parameter)
{
    unsigned slot=(unsigned)(uintptr_t)parameter;
    ac1_profile_guest(slot,0x100+slot,&eips[slot],&owner);
    if (!slot) WaitForSingleObject(stop,INFINITE);
    else {
        for(unsigned i=0;i<1000;++i) {
            ac1_profile_handoff((ac1_handoff_cause)(i%AC1_HANDOFF_CAUSES));
            ac1_profile_transfer(1200);
        }
        volatile uint64_t n=1;
        while(WaitForSingleObject(stop,0)!=WAIT_OBJECT_0)
            for(unsigned i=0;i<10000;++i) n=n*6364136223846793005ull+1;
    }
    ac1_profile_guest_done();
    return 0;
}
int main(void)
{
    ac1_profile_init();
    if (!ac1_profile_enabled) return 2;
    stop=CreateEventW(NULL,TRUE,FALSE,NULL);
    HANDLE threads[2];
    threads[0]=CreateThread(NULL,0,worker,(void *)(uintptr_t)0,0,NULL);
    threads[1]=CreateThread(NULL,0,worker,(void *)(uintptr_t)1,0,NULL);
    Sleep(1200);
    SetEvent(stop);
    WaitForMultipleObjects(2,threads,TRUE,INFINITE);
    CloseHandle(threads[0]); CloseHandle(threads[1]); CloseHandle(stop);
    ac1_profile_shutdown();
    puts("scheduler profiler fixture finished");
    return 0;
}
