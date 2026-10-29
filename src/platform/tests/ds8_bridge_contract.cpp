#define DIRECTSOUND_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../ac1_ds8_bridge.h"
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
constexpr uint32_t kBase=0x10000000u;
std::array<uint8_t, 4u*1024u*1024u> g_mem{};
uint32_t g_next=kBase+0x1000u;
HWND g_window=nullptr;
void *resolve(void*,uint32_t a,uint32_t n,uint32_t access){if(!n||(access!=AC1_DS8_GUEST_READ&&access!=AC1_DS8_GUEST_WRITE)||a<kBase)return nullptr;uint64_t o=uint64_t(a)-kBase;if(o+n>g_mem.size())return nullptr;return g_mem.data()+o;}
uint32_t alloc(void*,uint32_t n,uint32_t align){if(!n||!align||(align&(align-1)))return 0;g_next=(g_next+align-1)&~(align-1);uint32_t a=g_next;if(uint64_t(a)+n>uint64_t(kBase)+g_mem.size())return 0;g_next+=n;return a;}
void dealloc(void*,uint32_t){}
uintptr_t hwnd(void*,uint32_t h){return h==0x404u?reinterpret_cast<uintptr_t>(g_window):0;}
template<class T> bool put(uint32_t a,const T&v){void*p=resolve(nullptr,a,sizeof(T),AC1_DS8_GUEST_WRITE);if(!p)return false;std::memcpy(p,&v,sizeof(T));return true;}
template<class T> bool get(uint32_t a,T*v){void*p=resolve(nullptr,a,sizeof(T),AC1_DS8_GUEST_READ);if(!p||!v)return false;std::memcpy(v,p,sizeof(T));return true;}
uint32_t token(uint32_t kind,uint32_t slot){return AC1_DS8_TOKEN_BASE+kind*0x1000u+slot*0x10u;}
bool check(bool ok,const char*what){if(!ok)std::fprintf(stderr,"FAIL: %s\n",what);return ok;}
uint32_t fbits(float f){uint32_t b;std::memcpy(&b,&f,4);return b;}
int call(uint32_t kind,uint32_t slot,const uint32_t*a,uint32_t n,uint32_t*out){return ac1_ds8_dispatch(token(kind,slot),a,n,out);}
HWND make_window(){WNDCLASSW c{};c.lpfnWndProc=DefWindowProcW;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"AC1DS8BridgeContract";if(!RegisterClassW(&c)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return nullptr;return CreateWindowExW(0,c.lpszClassName,L"DS8 contract",WS_OVERLAPPED,0,0,32,32,nullptr,nullptr,c.hInstance,nullptr);}
#pragma pack(push,4)
struct GuestBufferDesc{uint32_t size,flags,bytes,reserved,wave;GUID algorithm;};
#pragma pack(pop)
}
int main(){
 const int counts[][24]={
  {3,1,1},
  {3,1,1,4,2,3,3,1,2,2,2,2},
  {3,1,1,2,3,4,2,2,2,2,3,8,4,2,2,2,2,2,1,5,1},
  {3,1,1,2,2,2,3,2,2,2,3,3,3,8,5,3,5,1},
  {3,1,1,2,3,4,2,2,2,2,3,8,4,2,2,2,2,2,1,5,1,4,4,5},
  {3,1,1,2,3,2,2,2,2,2,2,2,3,4,5,3,3,3,3,5,5},
  {3,1,1,3}
 };
 const uint32_t slots[]={3,12,21,18,24,21,4};
 for(uint32_t k=1;k<=7;++k)for(uint32_t s=0;s<slots[k-1];++s)if(!check(ac1_ds8_argument_count(token(k,s))==counts[k-1][s],"vtable slot argument count"))return 1;
 if(!check(ac1_ds8_argument_count(AC1_DS8_TOKEN_BASE)==0,"foreign token range")||!check(ac1_ds8_argument_count(token(7,4))==-1,"invalid slot diagnostic"))return 1;
 ac1_ds8_callbacks cb{};cb.struct_size=sizeof(cb);cb.guest_resolve=resolve;cb.guest_alloc=alloc;cb.guest_free=dealloc;cb.guest_hwnd_to_host=hwnd;
 if(!check(ac1_ds8_configure(&cb,nullptr)==1,"configure bridge"))return 1;
 uint32_t unsupported_result=0,notify_args[]={0,0,0},fx_args[]={0,0,0,0},path_args[]={0,0,0,0,0};if(!check(call(7,3,notify_args,3,&unsupported_result)==AC1_DS8_METHOD_UNSUPPORTED&&std::strstr(ac1_ds8_last_error(),"guest event handles")!=nullptr,"notify unsupported diagnostic"))return 1;
 if(!check(call(5,21,fx_args,4,&unsupported_result)==AC1_DS8_METHOD_UNSUPPORTED&&std::strstr(ac1_ds8_last_error(),"SetFX")!=nullptr,"SetFX unsupported diagnostic")||!check(call(5,23,path_args,5,&unsupported_result)==AC1_DS8_METHOD_UNSUPPORTED&&std::strstr(ac1_ds8_last_error(),"GetObjectInPath")!=nullptr,"GetObjectInPath unsupported diagnostic"))return 1;
 constexpr uint32_t out=kBase+0x100u;uint32_t hr=0;
 if(ac1_ds8_create8(0,out,0,&hr)!=AC1_DS8_HANDLED||static_cast<int32_t>(hr)<0){std::printf("DirectSound device unavailable; argument tables passed (%s, HRESULT 0x%08x)\n",ac1_ds8_last_error(),hr);ac1_ds8_shutdown();return 0;}
 uint32_t dev=0;if(!check(get(out,&dev)&&dev,"read guest device proxy"))return 1;
 g_window=make_window();if(!g_window){std::fprintf(stderr,"FAIL: create test HWND\n");ac1_ds8_shutdown();return 1;}
 uint32_t a_coop[]={dev,0x404u,DSSCL_PRIORITY};if(!check(call(2,6,a_coop,3,&hr)==AC1_DS8_HANDLED&&static_cast<int32_t>(hr)>=0,"SetCooperativeLevel")){DestroyWindow(g_window);ac1_ds8_shutdown();return 1;}
 constexpr uint32_t primary_desc=kBase+0x200u, primary_out=kBase+0x300u;
 GuestBufferDesc pd{sizeof(GuestBufferDesc),DSBCAPS_PRIMARYBUFFER|DSBCAPS_CTRL3D,0,0,0,{}};put(primary_desc,pd);uint32_t a_primary[]={dev,primary_desc,primary_out,0};
 if(!check(call(2,3,a_primary,4,&hr)==AC1_DS8_HANDLED&&static_cast<int32_t>(hr)>=0,"create primary buffer")){DestroyWindow(g_window);ac1_ds8_shutdown();return 1;}
 uint32_t primary=0;get(primary_out,&primary);
 constexpr uint32_t iid_listener_addr=kBase+0x400u,listener_out=kBase+0x420u;
 const GUID iid_listener={0x279AFA84u,0x4981u,0x11CEu,{0xA5,0x21,0,0x20,0xAF,0x0B,0xE5,0x60}};put(iid_listener_addr,iid_listener);uint32_t qi_listener[]={primary,iid_listener_addr,listener_out};
 if(!check(call(3,0,qi_listener,3,&hr)==AC1_DS8_HANDLED&&static_cast<int32_t>(hr)>=0,"query 3D listener")){DestroyWindow(g_window);ac1_ds8_shutdown();return 1;}uint32_t listener=0;get(listener_out,&listener);
 constexpr uint32_t wfaddr=kBase+0x500u,descaddr=kBase+0x600u,bufout=kBase+0x700u;
 WAVEFORMATEX wf{WAVE_FORMAT_PCM,1,22050,44100,2,16,0};put(wfaddr,wf);
 GuestBufferDesc sd{sizeof(GuestBufferDesc),DSBCAPS_CTRL3D|DSBCAPS_CTRLFREQUENCY|DSBCAPS_CTRLVOLUME|DSBCAPS_CTRLPOSITIONNOTIFY,8192,0,wfaddr,{0,0,0,{0,0,0,0,0,0,0,0}}};put(descaddr,sd);uint32_t a_sec[]={dev,descaddr,bufout,0};
 if(!check(call(2,3,a_sec,4,&hr)==AC1_DS8_HANDLED&&static_cast<int32_t>(hr)>=0,"create secondary 3D buffer")){DestroyWindow(g_window);ac1_ds8_shutdown();return 1;}uint32_t buffer=0;get(bufout,&buffer);
 constexpr uint32_t iid3addr=kBase+0x800u,out3=kBase+0x820u,iidnaddr=kBase+0x840u,outn=kBase+0x860u;
 const GUID iid3={0x279AFA86u,0x4981u,0x11CEu,{0xA5,0x21,0,0x20,0xAF,0x0B,0xE5,0x60}};const GUID iidn={0xB0210783u,0x89CDu,0x11D0u,{0xAF,8,0,0xA0,0xC9,0x25,0xCD,0x16}};put(iid3addr,iid3);put(iidnaddr,iidn);
 uint32_t q3[]={buffer,iid3addr,out3},qn[]={buffer,iidnaddr,outn};
 if(!check(call(3,0,q3,3,&hr)==AC1_DS8_HANDLED&&static_cast<int32_t>(hr)>=0,"query 3D buffer")||!check(call(3,0,qn,3,&hr)==AC1_DS8_HANDLED&&static_cast<int32_t>(hr)>=0,"query notify")){DestroyWindow(g_window);ac1_ds8_shutdown();return 1;}uint32_t b3=0,notify=0;get(out3,&b3);get(outn,&notify);
 constexpr uint32_t vec=kBase+0x900u,front=kBase+0x920u,top=kBase+0x940u,scalar=kBase+0x960u,caps=kBase+0x980u,statusp=kBase+0x9a0u,position=kBase+0x9c0u;
 D3DVECTOR v{1.25f,-2.5f,3.75f};uint32_t set_listener_pos[]={listener,fbits(v.x),fbits(v.y),fbits(v.z),DS3D_IMMEDIATE};
 if(!check(call(4,14,set_listener_pos,5,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr)),"listener SetPosition"))return 1;
 uint32_t get_listener_pos[]={listener,vec};if(!check(call(4,7,get_listener_pos,2,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr))&&get(vec,&v)&&v.x==1.25f&&v.y==-2.5f&&v.z==3.75f,"listener position round-trip"))return 1;
 uint32_t set_orient[]={listener,fbits(0),fbits(0),fbits(1),fbits(0),fbits(1),fbits(0),DS3D_IMMEDIATE};if(!check(call(4,13,set_orient,8,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr)),"listener SetOrientation"))return 1;uint32_t get_orient[]={listener,front,top};D3DVECTOR f{},t{};if(!check(call(4,6,get_orient,3,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr))&&get(front,&f)&&get(top,&t)&&f.z==1.0f&&t.y==1.0f,"listener orientation round-trip"))return 1;
 uint32_t setfactor[]={listener,fbits(2.0f),DS3D_IMMEDIATE};if(!check(call(4,11,setfactor,3,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr)),"listener SetDistanceFactor"))return 1;uint32_t getfactor[]={listener,scalar};if(!check(call(4,4,getfactor,2,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr))&&get(scalar,&v.x)&&v.x==2.0f,"listener distance factor round-trip"))return 1;
 uint32_t setdoppler[]={listener,fbits(1.5f),DS3D_IMMEDIATE},getdoppler[]={listener,scalar};if(!check(call(4,12,setdoppler,3,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr))&&call(4,5,getdoppler,2,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr))&&get(scalar,&v.x)&&v.x==1.5f,"listener doppler factor round-trip"))return 1;
 uint32_t setrolloff[]={listener,fbits(0.75f),DS3D_IMMEDIATE},getrolloff[]={listener,scalar};if(!check(call(4,15,setrolloff,3,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr))&&call(4,8,getrolloff,2,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr))&&get(scalar,&v.x)&&v.x==0.75f,"listener rolloff factor round-trip"))return 1;
 uint32_t commit[]={listener};if(!check(call(4,17,commit,1,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr)),"CommitDeferredSettings"))return 1;
 uint32_t setbufpos[]={b3,fbits(4),fbits(5),fbits(6),DS3D_IMMEDIATE};if(!check(call(6,19,setbufpos,5,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr)),"3D buffer SetPosition"))return 1;uint32_t getbufpos[]={b3,position};D3DVECTOR bv{};if(!check(call(6,10,getbufpos,2,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr))&&get(position,&bv)&&bv.x==4.0f&&bv.y==5.0f&&bv.z==6.0f,"3D buffer position round-trip"))return 1;
 uint32_t setmin[]={b3,fbits(2),DS3D_IMMEDIATE},getmin[]={b3,scalar};if(!check(call(6,17,setmin,3,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr))&&call(6,8,getmin,2,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr))&&get(scalar,&v.x)&&v.x==2.0f,"3D buffer min distance round-trip"))return 1;
 uint32_t setmax[]={b3,fbits(500),DS3D_IMMEDIATE},getmax[]={b3,scalar};if(!check(call(6,16,setmax,3,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr))&&call(6,7,getmax,2,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr))&&get(scalar,&v.x)&&v.x==500.0f,"3D buffer max distance round-trip"))return 1;
 uint32_t setmode[]={b3,DS3DMODE_NORMAL,DS3D_IMMEDIATE},getmode[]={b3,scalar};DWORD mode=0;if(!check(call(6,18,setmode,3,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr))&&call(6,9,getmode,2,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr))&&get(scalar,&mode)&&mode==DS3DMODE_NORMAL,"3D buffer mode round-trip"))return 1;
 DSBCAPS bc{};bc.dwSize=sizeof(bc);put(caps,bc);uint32_t getcaps[]={buffer,caps},getstatus[]={buffer,statusp},setfreq[]={buffer,22050},getfreq[]={buffer,scalar},stop[]={buffer};if(!check(call(3,3,getcaps,2,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr)),"buffer GetCaps")||!check(call(3,9,getstatus,2,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr)),"buffer GetStatus")||!check(call(3,17,setfreq,2,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr))&&call(3,8,getfreq,2,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr)),"buffer frequency round-trip")||!check(call(3,18,stop,1,&hr)==AC1_DS8_HANDLED,"buffer Stop"))return 1;
 uint32_t dupout=kBase+0x9e0u,duplicate_args[]={dev,buffer,dupout};if(!check(call(2,5,duplicate_args,3,&hr)==AC1_DS8_HANDLED&&SUCCEEDED(static_cast<HRESULT>(hr)),"DuplicateSoundBuffer"))return 1;
 DestroyWindow(g_window);g_window=nullptr;ac1_ds8_shutdown();std::puts("DirectSound bridge contract passed with native device and 3D round-trips.");return 0;
}
