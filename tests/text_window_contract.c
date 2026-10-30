/* Exercise the real Windows or Android window adapter's nested ToUnicode call. */
#define main ac1_window_base_smoke_main
#include "ac1_window_win32_smoke.c"
#undef main
#include "ac1_input_script.h"
#ifdef TEXT_PORTABLE
uintptr_t ac1_android_d3d9_guest_hwnd(void *c,uint32_t h){(void)c;(void)h;return 1;}
void ac1_android_surface_size(int32_t *w,int32_t *h){*w=1280;*h=720;}
#endif
static unsigned chars, controls;
static uint64_t frame;
uint64_t ac1_d3d9_present_count(void){return frame;}
static int text_callback(void *opaque,CPU *cpu,uint32_t procedure,uint32_t hwnd,
                         uint32_t message,uint32_t wp,uint32_t lp,uint32_t *result) {
    smoke_context *context=opaque;
    if(message==WM_KEYDOWN && wp==0xe7) {
        CPU nested;
        uint32_t keys=0x14000,args[]={wp,0,keys,0x15000,16,0};
        if(!call_api(context,&nested,"GetKeyboardState",4,&keys,1,0))return 0;
        if(!call_api(context,&nested,"ToUnicode",24,args,6,0) || nested.eax!=1)return 0;
        uint16_t value=*(uint16_t *)guest_ptr(0x15000,2);
        if(value!=(chars==0?'A':0xef))return 0;
        ++chars;
    }
    if(message==WM_KEYDOWN && (wp==8||wp==13||wp==27))++controls;
    return invoke_guest_wndproc(opaque,cpu,procedure,hwnd,message,wp,lp,result);
}
int main(void) {
    FILE *f=fopen("work/gamepad/text/window-replay.txt","wb");CHECK(f);
    fputs("0 text 0x41 0xef\n0 textkey BACKSPACE\n0 textkey ENTER\n0 textkey ESCAPE\n",f);fclose(f);
    _putenv_s("AC1_INPUT_SCRIPT","work/gamepad/text/window-replay.txt");
    _putenv_s("AC1_INPUT_RECORD","");
    guest_memory_bind(guest_bytes,GUEST_BASE,sizeof(guest_bytes));
    smoke_context context={0};context.next_allocation=0x70000;
    ac1_window_win32_callbacks cb={&context,memory_read,memory_write,memory_allocate,memory_free,text_callback,guest_thread_id};
    char error[512];CPU cpu;context.window=ac1_window_win32_create(&cb,error,sizeof(error));CHECK(context.window);
    uint32_t hwnd;
#ifdef TEXT_PORTABLE
    // The smoke fixture's default host cursor is platform-specific.
    uint32_t cursor_args[]={0,32512};CHECK(call_api(&context,&cpu,"LoadCursorA",8,cursor_args,2,0));
    uint32_t *klass=(uint32_t *)guest_ptr(GUEST_CLASS,48);
    memset(klass,0,48);klass[0]=48;klass[2]=0x004013e3;klass[7]=cpu.eax;klass[10]=GUEST_CLASS_NAME;
    strcpy((char *)guest_ptr(GUEST_CLASS_NAME,32),"text fixture");
    uint32_t class_arg=GUEST_CLASS;CHECK(call_api(&context,&cpu,"RegisterClassExA",4,&class_arg,1,0)&&cpu.eax);
#else
    CHECK(register_guest_class(&context,&cpu));
#endif
    CHECK(create_guest_window(&context,&cpu,&hwnd));
    uint32_t peek[]={GUEST_MSG,hwnd,0,0,PM_REMOVE},arg=GUEST_MSG;
    unsigned drained=0;
    while(frame<20) {
        CHECK(call_api(&context,&cpu,"PeekMessageA",20,peek,5,1));if(!cpu.eax){++frame;continue;}
        CHECK(++drained<40);CHECK(call_api(&context,&cpu,"DispatchMessageA",4,&arg,1,0));
    }
    CHECK(chars==2&&controls==3);
    ac1_window_win32_destroy(context.window);guest_memory_unbind();
    puts("PASS: actual window adapter dispatches supplied Unicode through nested ToUnicode; Backspace/Enter/Escape remain key messages");return 0;
}
