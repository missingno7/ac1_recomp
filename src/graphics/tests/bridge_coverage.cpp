#include "ac1_d3d9_bridge.h"
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint32_t base = 0x10000;
std::vector<uint8_t> memory(32 * 1024 * 1024);
std::map<uint32_t,uint32_t> allocations;
uint32_t next_address = base;
HWND window = nullptr;

void require(bool ok, const char *message)
{ if (!ok) throw std::runtime_error(std::string(message) + ": " + ac1_d3d9_last_error()); }
void *resolve(void *, uint32_t address, uint32_t size, uint32_t)
{
    if (address < base || uint64_t{address} + size > uint64_t{base} + memory.size()) return nullptr;
    return memory.data() + address - base;
}
uint32_t allocate(void *, uint32_t size, uint32_t alignment)
{
    const uint64_t address = (uint64_t{next_address} + alignment - 1) & ~(uint64_t{alignment} - 1);
    if (address + size > uint64_t{base} + memory.size()) return 0;
    next_address = static_cast<uint32_t>(address + size);
    allocations[static_cast<uint32_t>(address)] = size;
    return static_cast<uint32_t>(address);
}
void free_memory(void *, uint32_t address) { allocations.erase(address); }
uintptr_t hwnd(void *, uint32_t guest) { return guest == 0x1234 ? reinterpret_cast<uintptr_t>(window) : 0; }
void log(void *, const char *message) { std::fprintf(stderr,"bridge: %s\n",message); }
template<class T> uint32_t put(const T &value)
{
    const uint32_t address = allocate(nullptr,sizeof(T),4);
    require(address != 0,"test guest allocation");
    std::memcpy(resolve(nullptr,address,sizeof(T),0),&value,sizeof(T));
    return address;
}
template<class T> T get(uint32_t address)
{
    T value{}; auto *p = resolve(nullptr,address,sizeof(T),0); require(p != nullptr,"test guest read");
    std::memcpy(&value,p,sizeof(T)); return value;
}
uint32_t word(uint32_t address) { return get<uint32_t>(address); }
uint32_t token(uint32_t object, uint32_t slot) { return word(word(object)+slot*4); }
uint32_t call(uint32_t object, uint32_t slot, std::initializer_list<uint32_t> parameters = {})
{
    std::vector<uint32_t> args{object}; args.insert(args.end(),parameters.begin(),parameters.end());
    uint32_t result = 0xdeadbeef;
    require(ac1_d3d9_argument_count(token(object,slot)) == static_cast<int>(args.size()),"method arity");
    const int handled = ac1_d3d9_dispatch(token(object,slot),args.data(),static_cast<uint32_t>(args.size()),&result);
    if (handled != 1) throw std::runtime_error("dispatch slot " + std::to_string(slot) + ": " + ac1_d3d9_last_error());
    return result;
}
void hr(uint32_t object,uint32_t slot,std::initializer_list<uint32_t> parameters = {})
{
    const uint32_t result = call(object,slot,parameters);
    if (FAILED(static_cast<HRESULT>(result))) {
        char code[32]; std::snprintf(code,sizeof(code)," HRESULT 0x%08x",result);
        throw std::runtime_error("native slot " + std::to_string(slot) + code + ": " + ac1_d3d9_last_error());
    }
}
uint32_t make(uint32_t object,uint32_t slot,std::initializer_list<uint32_t> before,
              std::initializer_list<uint32_t> after = {})
{
    const uint32_t out = put(uint32_t{0});
    std::vector<uint32_t> args{object}; args.insert(args.end(),before.begin(),before.end());
    args.push_back(out); args.insert(args.end(),after.begin(),after.end());
    uint32_t result = 0;
    require(ac1_d3d9_dispatch(token(object,slot),args.data(),static_cast<uint32_t>(args.size()),&result)==1 &&
        SUCCEEDED(static_cast<HRESULT>(result)) && word(out),"interface out parameter");
    return word(out);
}
void release(uint32_t object) { call(object,2); }
uint32_t bits(float f) { uint32_t value; std::memcpy(&value,&f,4); return value; }

// The expected stack widths come from the SDK's typed COM signatures, not the
// bridge's table, and cover every inherited slot and malformed boundary token.
template<class R,class C,class... A> constexpr unsigned words(R (STDMETHODCALLTYPE C::*)(A...)) { return 1+sizeof...(A); }
template<size_t N> void check_slots(uint32_t kind,const unsigned (&expected)[N])
{
    for (uint32_t slot=0;slot<N;++slot) {
        const uint32_t t=0x7e000000+kind*0x1000+slot*0x10;
        const int count=ac1_d3d9_argument_count(t);
        require(count == ((kind==1 && slot==3) ? -2 : static_cast<int>(expected[slot])),"SDK slot arity coverage");
        require(ac1_d3d9_argument_count(t+1)==-1,"unaligned token rejected");
    }
    require(ac1_d3d9_argument_count(0x7e000000+kind*0x1000+N*0x10)==-1,"past-end slot rejected");
}
void check_arities()
{
    {
        const unsigned expected[] = {
            words(&IDirect3D9::QueryInterface),
            words(&IDirect3D9::AddRef),
            words(&IDirect3D9::Release),
            words(&IDirect3D9::RegisterSoftwareDevice),
            words(&IDirect3D9::GetAdapterCount),
            words(&IDirect3D9::GetAdapterIdentifier),
            words(&IDirect3D9::GetAdapterModeCount),
            words(&IDirect3D9::EnumAdapterModes),
            words(&IDirect3D9::GetAdapterDisplayMode),
            words(&IDirect3D9::CheckDeviceType),
            words(&IDirect3D9::CheckDeviceFormat),
            words(&IDirect3D9::CheckDeviceMultiSampleType),
            words(&IDirect3D9::CheckDepthStencilMatch),
            words(&IDirect3D9::CheckDeviceFormatConversion),
            words(&IDirect3D9::GetDeviceCaps),
            words(&IDirect3D9::GetAdapterMonitor),
            words(&IDirect3D9::CreateDevice)
        };
        check_slots(1,expected);
    }
    {
        const unsigned expected[] = {
            words(&IDirect3DDevice9::QueryInterface),
            words(&IDirect3DDevice9::AddRef),
            words(&IDirect3DDevice9::Release),
            words(&IDirect3DDevice9::TestCooperativeLevel),
            words(&IDirect3DDevice9::GetAvailableTextureMem),
            words(&IDirect3DDevice9::EvictManagedResources),
            words(&IDirect3DDevice9::GetDirect3D),
            words(&IDirect3DDevice9::GetDeviceCaps),
            words(&IDirect3DDevice9::GetDisplayMode),
            words(&IDirect3DDevice9::GetCreationParameters),
            words(&IDirect3DDevice9::SetCursorProperties),
            words(&IDirect3DDevice9::SetCursorPosition),
            words(&IDirect3DDevice9::ShowCursor),
            words(&IDirect3DDevice9::CreateAdditionalSwapChain),
            words(&IDirect3DDevice9::GetSwapChain),
            words(&IDirect3DDevice9::GetNumberOfSwapChains),
            words(&IDirect3DDevice9::Reset),
            words(&IDirect3DDevice9::Present),
            words(&IDirect3DDevice9::GetBackBuffer),
            words(&IDirect3DDevice9::GetRasterStatus),
            words(&IDirect3DDevice9::SetDialogBoxMode),
            words(&IDirect3DDevice9::SetGammaRamp),
            words(&IDirect3DDevice9::GetGammaRamp),
            words(&IDirect3DDevice9::CreateTexture),
            words(&IDirect3DDevice9::CreateVolumeTexture),
            words(&IDirect3DDevice9::CreateCubeTexture),
            words(&IDirect3DDevice9::CreateVertexBuffer),
            words(&IDirect3DDevice9::CreateIndexBuffer),
            words(&IDirect3DDevice9::CreateRenderTarget),
            words(&IDirect3DDevice9::CreateDepthStencilSurface),
            words(&IDirect3DDevice9::UpdateSurface),
            words(&IDirect3DDevice9::UpdateTexture),
            words(&IDirect3DDevice9::GetRenderTargetData),
            words(&IDirect3DDevice9::GetFrontBufferData),
            words(&IDirect3DDevice9::StretchRect),
            words(&IDirect3DDevice9::ColorFill),
            words(&IDirect3DDevice9::CreateOffscreenPlainSurface),
            words(&IDirect3DDevice9::SetRenderTarget),
            words(&IDirect3DDevice9::GetRenderTarget),
            words(&IDirect3DDevice9::SetDepthStencilSurface),
            words(&IDirect3DDevice9::GetDepthStencilSurface),
            words(&IDirect3DDevice9::BeginScene),
            words(&IDirect3DDevice9::EndScene),
            words(&IDirect3DDevice9::Clear),
            words(&IDirect3DDevice9::SetTransform),
            words(&IDirect3DDevice9::GetTransform),
            words(&IDirect3DDevice9::MultiplyTransform),
            words(&IDirect3DDevice9::SetViewport),
            words(&IDirect3DDevice9::GetViewport),
            words(&IDirect3DDevice9::SetMaterial),
            words(&IDirect3DDevice9::GetMaterial),
            words(&IDirect3DDevice9::SetLight),
            words(&IDirect3DDevice9::GetLight),
            words(&IDirect3DDevice9::LightEnable),
            words(&IDirect3DDevice9::GetLightEnable),
            words(&IDirect3DDevice9::SetClipPlane),
            words(&IDirect3DDevice9::GetClipPlane),
            words(&IDirect3DDevice9::SetRenderState),
            words(&IDirect3DDevice9::GetRenderState),
            words(&IDirect3DDevice9::CreateStateBlock),
            words(&IDirect3DDevice9::BeginStateBlock),
            words(&IDirect3DDevice9::EndStateBlock),
            words(&IDirect3DDevice9::SetClipStatus),
            words(&IDirect3DDevice9::GetClipStatus),
            words(&IDirect3DDevice9::GetTexture),
            words(&IDirect3DDevice9::SetTexture),
            words(&IDirect3DDevice9::GetTextureStageState),
            words(&IDirect3DDevice9::SetTextureStageState),
            words(&IDirect3DDevice9::GetSamplerState),
            words(&IDirect3DDevice9::SetSamplerState),
            words(&IDirect3DDevice9::ValidateDevice),
            words(&IDirect3DDevice9::SetPaletteEntries),
            words(&IDirect3DDevice9::GetPaletteEntries),
            words(&IDirect3DDevice9::SetCurrentTexturePalette),
            words(&IDirect3DDevice9::GetCurrentTexturePalette),
            words(&IDirect3DDevice9::SetScissorRect),
            words(&IDirect3DDevice9::GetScissorRect),
            words(&IDirect3DDevice9::SetSoftwareVertexProcessing),
            words(&IDirect3DDevice9::GetSoftwareVertexProcessing),
            words(&IDirect3DDevice9::SetNPatchMode),
            words(&IDirect3DDevice9::GetNPatchMode),
            words(&IDirect3DDevice9::DrawPrimitive),
            words(&IDirect3DDevice9::DrawIndexedPrimitive),
            words(&IDirect3DDevice9::DrawPrimitiveUP),
            words(&IDirect3DDevice9::DrawIndexedPrimitiveUP),
            words(&IDirect3DDevice9::ProcessVertices),
            words(&IDirect3DDevice9::CreateVertexDeclaration),
            words(&IDirect3DDevice9::SetVertexDeclaration),
            words(&IDirect3DDevice9::GetVertexDeclaration),
            words(&IDirect3DDevice9::SetFVF),
            words(&IDirect3DDevice9::GetFVF),
            words(&IDirect3DDevice9::CreateVertexShader),
            words(&IDirect3DDevice9::SetVertexShader),
            words(&IDirect3DDevice9::GetVertexShader),
            words(&IDirect3DDevice9::SetVertexShaderConstantF),
            words(&IDirect3DDevice9::GetVertexShaderConstantF),
            words(&IDirect3DDevice9::SetVertexShaderConstantI),
            words(&IDirect3DDevice9::GetVertexShaderConstantI),
            words(&IDirect3DDevice9::SetVertexShaderConstantB),
            words(&IDirect3DDevice9::GetVertexShaderConstantB),
            words(&IDirect3DDevice9::SetStreamSource),
            words(&IDirect3DDevice9::GetStreamSource),
            words(&IDirect3DDevice9::SetStreamSourceFreq),
            words(&IDirect3DDevice9::GetStreamSourceFreq),
            words(&IDirect3DDevice9::SetIndices),
            words(&IDirect3DDevice9::GetIndices),
            words(&IDirect3DDevice9::CreatePixelShader),
            words(&IDirect3DDevice9::SetPixelShader),
            words(&IDirect3DDevice9::GetPixelShader),
            words(&IDirect3DDevice9::SetPixelShaderConstantF),
            words(&IDirect3DDevice9::GetPixelShaderConstantF),
            words(&IDirect3DDevice9::SetPixelShaderConstantI),
            words(&IDirect3DDevice9::GetPixelShaderConstantI),
            words(&IDirect3DDevice9::SetPixelShaderConstantB),
            words(&IDirect3DDevice9::GetPixelShaderConstantB),
            words(&IDirect3DDevice9::DrawRectPatch),
            words(&IDirect3DDevice9::DrawTriPatch),
            words(&IDirect3DDevice9::DeletePatch),
            words(&IDirect3DDevice9::CreateQuery)
        };
        check_slots(2,expected);
    }
    {
        const unsigned expected[] = {
            words(&IDirect3DSurface9::QueryInterface),
            words(&IDirect3DSurface9::AddRef),
            words(&IDirect3DSurface9::Release),
            words(&IDirect3DSurface9::GetDevice),
            words(&IDirect3DSurface9::SetPrivateData),
            words(&IDirect3DSurface9::GetPrivateData),
            words(&IDirect3DSurface9::FreePrivateData),
            words(&IDirect3DSurface9::SetPriority),
            words(&IDirect3DSurface9::GetPriority),
            words(&IDirect3DSurface9::PreLoad),
            words(&IDirect3DSurface9::GetType),
            words(&IDirect3DSurface9::GetContainer),
            words(&IDirect3DSurface9::GetDesc),
            words(&IDirect3DSurface9::LockRect),
            words(&IDirect3DSurface9::UnlockRect),
            words(&IDirect3DSurface9::GetDC),
            words(&IDirect3DSurface9::ReleaseDC)
        };
        check_slots(3,expected);
    }
    {
        const unsigned expected[] = {
            words(&IDirect3DVertexBuffer9::QueryInterface),
            words(&IDirect3DVertexBuffer9::AddRef),
            words(&IDirect3DVertexBuffer9::Release),
            words(&IDirect3DVertexBuffer9::GetDevice),
            words(&IDirect3DVertexBuffer9::SetPrivateData),
            words(&IDirect3DVertexBuffer9::GetPrivateData),
            words(&IDirect3DVertexBuffer9::FreePrivateData),
            words(&IDirect3DVertexBuffer9::SetPriority),
            words(&IDirect3DVertexBuffer9::GetPriority),
            words(&IDirect3DVertexBuffer9::PreLoad),
            words(&IDirect3DVertexBuffer9::GetType),
            words(&IDirect3DVertexBuffer9::Lock),
            words(&IDirect3DVertexBuffer9::Unlock),
            words(&IDirect3DVertexBuffer9::GetDesc)
        };
        check_slots(4,expected);
    }
    {
        const unsigned expected[] = {
            words(&IDirect3DIndexBuffer9::QueryInterface),
            words(&IDirect3DIndexBuffer9::AddRef),
            words(&IDirect3DIndexBuffer9::Release),
            words(&IDirect3DIndexBuffer9::GetDevice),
            words(&IDirect3DIndexBuffer9::SetPrivateData),
            words(&IDirect3DIndexBuffer9::GetPrivateData),
            words(&IDirect3DIndexBuffer9::FreePrivateData),
            words(&IDirect3DIndexBuffer9::SetPriority),
            words(&IDirect3DIndexBuffer9::GetPriority),
            words(&IDirect3DIndexBuffer9::PreLoad),
            words(&IDirect3DIndexBuffer9::GetType),
            words(&IDirect3DIndexBuffer9::Lock),
            words(&IDirect3DIndexBuffer9::Unlock),
            words(&IDirect3DIndexBuffer9::GetDesc)
        };
        check_slots(5,expected);
    }
    {
        const unsigned expected[] = {
            words(&IDirect3DVertexDeclaration9::QueryInterface),
            words(&IDirect3DVertexDeclaration9::AddRef),
            words(&IDirect3DVertexDeclaration9::Release),
            words(&IDirect3DVertexDeclaration9::GetDevice),
            words(&IDirect3DVertexDeclaration9::GetDeclaration)
        };
        check_slots(6,expected);
    }
    {
        const unsigned expected[] = {
            words(&IDirect3DTexture9::QueryInterface),
            words(&IDirect3DTexture9::AddRef),
            words(&IDirect3DTexture9::Release),
            words(&IDirect3DTexture9::GetDevice),
            words(&IDirect3DTexture9::SetPrivateData),
            words(&IDirect3DTexture9::GetPrivateData),
            words(&IDirect3DTexture9::FreePrivateData),
            words(&IDirect3DTexture9::SetPriority),
            words(&IDirect3DTexture9::GetPriority),
            words(&IDirect3DTexture9::PreLoad),
            words(&IDirect3DTexture9::GetType),
            words(&IDirect3DTexture9::SetLOD),
            words(&IDirect3DTexture9::GetLOD),
            words(&IDirect3DTexture9::GetLevelCount),
            words(&IDirect3DTexture9::SetAutoGenFilterType),
            words(&IDirect3DTexture9::GetAutoGenFilterType),
            words(&IDirect3DTexture9::GenerateMipSubLevels),
            words(&IDirect3DTexture9::GetLevelDesc),
            words(&IDirect3DTexture9::GetSurfaceLevel),
            words(&IDirect3DTexture9::LockRect),
            words(&IDirect3DTexture9::UnlockRect),
            words(&IDirect3DTexture9::AddDirtyRect)
        };
        check_slots(7,expected);
    }
    {
        const unsigned expected[] = {
            words(&IDirect3DPixelShader9::QueryInterface),
            words(&IDirect3DPixelShader9::AddRef),
            words(&IDirect3DPixelShader9::Release),
            words(&IDirect3DPixelShader9::GetDevice),
            words(&IDirect3DPixelShader9::GetFunction)
        };
        check_slots(8,expected);
    }
    {
        const unsigned expected[] = {
            words(&IDirect3DVertexShader9::QueryInterface),
            words(&IDirect3DVertexShader9::AddRef),
            words(&IDirect3DVertexShader9::Release),
            words(&IDirect3DVertexShader9::GetDevice),
            words(&IDirect3DVertexShader9::GetFunction)
        };
        check_slots(9,expected);
    }
    {
        const unsigned expected[] = {
            words(&IDirect3DCubeTexture9::QueryInterface),
            words(&IDirect3DCubeTexture9::AddRef),
            words(&IDirect3DCubeTexture9::Release),
            words(&IDirect3DCubeTexture9::GetDevice),
            words(&IDirect3DCubeTexture9::SetPrivateData),
            words(&IDirect3DCubeTexture9::GetPrivateData),
            words(&IDirect3DCubeTexture9::FreePrivateData),
            words(&IDirect3DCubeTexture9::SetPriority),
            words(&IDirect3DCubeTexture9::GetPriority),
            words(&IDirect3DCubeTexture9::PreLoad),
            words(&IDirect3DCubeTexture9::GetType),
            words(&IDirect3DCubeTexture9::SetLOD),
            words(&IDirect3DCubeTexture9::GetLOD),
            words(&IDirect3DCubeTexture9::GetLevelCount),
            words(&IDirect3DCubeTexture9::SetAutoGenFilterType),
            words(&IDirect3DCubeTexture9::GetAutoGenFilterType),
            words(&IDirect3DCubeTexture9::GenerateMipSubLevels),
            words(&IDirect3DCubeTexture9::GetLevelDesc),
            words(&IDirect3DCubeTexture9::GetCubeMapSurface),
            words(&IDirect3DCubeTexture9::LockRect),
            words(&IDirect3DCubeTexture9::UnlockRect),
            words(&IDirect3DCubeTexture9::AddDirtyRect)
        };
        check_slots(11,expected);
    }
    {
        const unsigned expected[] = {
            words(&IDirect3DQuery9::QueryInterface),
            words(&IDirect3DQuery9::AddRef),
            words(&IDirect3DQuery9::Release),
            words(&IDirect3DQuery9::GetDevice),
            words(&IDirect3DQuery9::GetType),
            words(&IDirect3DQuery9::GetDataSize),
            words(&IDirect3DQuery9::Issue),
            words(&IDirect3DQuery9::GetData)
        };
        check_slots(12,expected);
    }
    {
        const unsigned expected[] = {
            words(&IDirect3DVolumeTexture9::QueryInterface),
            words(&IDirect3DVolumeTexture9::AddRef),
            words(&IDirect3DVolumeTexture9::Release),
            words(&IDirect3DVolumeTexture9::GetDevice),
            words(&IDirect3DVolumeTexture9::SetPrivateData),
            words(&IDirect3DVolumeTexture9::GetPrivateData),
            words(&IDirect3DVolumeTexture9::FreePrivateData),
            words(&IDirect3DVolumeTexture9::SetPriority),
            words(&IDirect3DVolumeTexture9::GetPriority),
            words(&IDirect3DVolumeTexture9::PreLoad),
            words(&IDirect3DVolumeTexture9::GetType),
            words(&IDirect3DVolumeTexture9::SetLOD),
            words(&IDirect3DVolumeTexture9::GetLOD),
            words(&IDirect3DVolumeTexture9::GetLevelCount),
            words(&IDirect3DVolumeTexture9::SetAutoGenFilterType),
            words(&IDirect3DVolumeTexture9::GetAutoGenFilterType),
            words(&IDirect3DVolumeTexture9::GenerateMipSubLevels),
            words(&IDirect3DVolumeTexture9::GetLevelDesc),
            words(&IDirect3DVolumeTexture9::GetVolumeLevel),
            words(&IDirect3DVolumeTexture9::LockBox),
            words(&IDirect3DVolumeTexture9::UnlockBox),
            words(&IDirect3DVolumeTexture9::AddDirtyBox)
        };
        check_slots(13,expected);
    }
    {
        const unsigned expected[] = {
            words(&IDirect3DVolume9::QueryInterface),
            words(&IDirect3DVolume9::AddRef),
            words(&IDirect3DVolume9::Release),
            words(&IDirect3DVolume9::GetDevice),
            words(&IDirect3DVolume9::SetPrivateData),
            words(&IDirect3DVolume9::GetPrivateData),
            words(&IDirect3DVolume9::FreePrivateData),
            words(&IDirect3DVolume9::GetContainer),
            words(&IDirect3DVolume9::GetDesc),
            words(&IDirect3DVolume9::LockBox),
            words(&IDirect3DVolume9::UnlockBox)
        };
        check_slots(14,expected);
    }
    {
        const unsigned expected[] = {
            words(&IDirect3DStateBlock9::QueryInterface),
            words(&IDirect3DStateBlock9::AddRef),
            words(&IDirect3DStateBlock9::Release),
            words(&IDirect3DStateBlock9::GetDevice),
            words(&IDirect3DStateBlock9::Capture),
            words(&IDirect3DStateBlock9::Apply)
        };
        check_slots(15,expected);
    }
    {
        const unsigned expected[] = {
            words(&IDirect3DSwapChain9::QueryInterface),
            words(&IDirect3DSwapChain9::AddRef),
            words(&IDirect3DSwapChain9::Release),
            words(&IDirect3DSwapChain9::Present),
            words(&IDirect3DSwapChain9::GetFrontBufferData),
            words(&IDirect3DSwapChain9::GetBackBuffer),
            words(&IDirect3DSwapChain9::GetRasterStatus),
            words(&IDirect3DSwapChain9::GetDisplayMode),
            words(&IDirect3DSwapChain9::GetDevice),
            words(&IDirect3DSwapChain9::GetPresentParameters)
        };
        check_slots(16,expected);
    }
    require(ac1_d3d9_argument_count(0x12345678)==0,"foreign token");
    require(ac1_d3d9_argument_count(0x7e011000)==0,"foreign kind");
    // D3DX buffer pointers and matrix-pointer arrays still require adapters.
    const int d3dx[] = {3,1,1,-2,-2,2,4,2,3,3,3,2,5,4,5,4,5,4,5,4,5,4,5,-2,4,5,-2};
    for (uint32_t slot=0;slot<27;++slot) require(ac1_d3d9_argument_count(0x7e00a000+slot*16)==d3dx[slot],"D3DX extension arities");
    std::puts("SDK arity coverage passed: all 16 kinds, including 119 device slots");
}

#pragma pack(push,4)
struct Present { uint32_t width=320,height=240,format=D3DFMT_A8R8G8B8,count=1,ms=0,quality=0,swap=D3DSWAPEFFECT_DISCARD,hwnd=0x1234; int32_t windowed=1,auto_depth=1; uint32_t depth=D3DFMT_D24S8,flags=0,refresh=0,interval=D3DPRESENT_INTERVAL_IMMEDIATE; };
struct LockedBox { int32_t row,slice; uint32_t bits; };
struct LockedRect { int32_t pitch; uint32_t bits; };
#pragma pack(pop)
static_assert(sizeof(Present)==56 && sizeof(LockedBox)==12 && sizeof(LockedRect)==8,"PE32 structs");

void get_device(uint32_t object,uint32_t slot,uint32_t device)
{
    const uint32_t got=make(object,slot,{}); require(got==device,"GetDevice identity"); release(got);
}
template<class T> void roundtrip(uint32_t device,uint32_t set_slot,uint32_t get_slot,
                                  const T &value,std::initializer_list<uint32_t> prefix = {})
{
    const uint32_t in=put(value),out=put(T{});
    std::vector<uint32_t> set{device}; set.insert(set.end(),prefix.begin(),prefix.end()); set.push_back(in);
    auto read=set; read.back()=out;
    uint32_t result;
    require(ac1_d3d9_dispatch(token(device,set_slot),set.data(),static_cast<uint32_t>(set.size()),&result)==1 && SUCCEEDED(static_cast<HRESULT>(result)),"state set");
    require(ac1_d3d9_dispatch(token(device,get_slot),read.data(),static_cast<uint32_t>(read.size()),&result)==1 && SUCCEEDED(static_cast<HRESULT>(result)),"state get");
    const T actual=get<T>(out); require(std::memcmp(&actual,&value,sizeof(T))==0,"state round trip bytes");
}
void states(uint32_t device)
{
    D3DMATRIX matrix{}; matrix._11=2; matrix._22=3; matrix._33=4; matrix._44=1;
    roundtrip(device,44,45,matrix,{D3DTS_WORLD});
    const uint32_t unaligned=allocate(nullptr,sizeof(matrix)+1,4)+1;
    std::memcpy(resolve(nullptr,unaligned,sizeof(matrix),0),&matrix,sizeof(matrix));
    hr(device,44,{D3DTS_WORLD,unaligned});
    std::memset(resolve(nullptr,unaligned,sizeof(matrix),0),0,sizeof(matrix));
    hr(device,45,{D3DTS_WORLD,unaligned});
    require(std::memcmp(resolve(nullptr,unaligned,sizeof(matrix),0),&matrix,sizeof(matrix))==0,"unaligned PE32 matrix copying");
    D3DMATRIX identity{}; identity._11=identity._22=identity._33=identity._44=1;
    hr(device,46,{D3DTS_WORLD,put(identity)});
    const uint32_t out=put(D3DMATRIX{}); hr(device,45,{D3DTS_WORLD,out});
    require(std::memcmp(resolve(nullptr,out,sizeof(matrix),0),&matrix,sizeof(matrix))==0,"MultiplyTransform");
    hr(device,44,{D3DTS_WORLD,put(identity)});
    roundtrip(device,47,48,D3DVIEWPORT9{0,0,320,240,0,1});
    D3DMATERIAL9 material{}; material.Diffuse={0.2f,0.4f,0.6f,1}; material.Ambient={0.1f,0.1f,0.1f,1}; material.Power=8;
    roundtrip(device,49,50,material);
    D3DLIGHT9 light{}; light.Type=D3DLIGHT_DIRECTIONAL; light.Diffuse={1,0.5f,0.25f,1}; light.Direction={0,0,1};
    roundtrip(device,51,52,light,{0}); hr(device,53,{0,TRUE});
    const uint32_t scalar=put(uint32_t{0}); hr(device,54,{0,scalar}); require(word(scalar)!=0,"light enabled readback");
    roundtrip(device,55,56,std::array<float,4>{1,0,0,0},{0});
    roundtrip(device,75,76,RECT{2,3,300,220});
    roundtrip(device,62,63,D3DCLIPSTATUS9{0,0});
    hr(device,57,{D3DRS_ALPHABLENDENABLE,FALSE}); hr(device,58,{D3DRS_ALPHABLENDENABLE,scalar}); require(word(scalar)==FALSE,"render state");
    hr(device,69,{0,D3DSAMP_MINFILTER,D3DTEXF_POINT}); hr(device,68,{0,D3DSAMP_MINFILTER,scalar}); require(word(scalar)==D3DTEXF_POINT,"sampler state");
    hr(device,67,{0,D3DTSS_COLOROP,D3DTOP_SELECTARG1}); hr(device,66,{0,D3DTSS_COLOROP,scalar}); require(word(scalar)==D3DTOP_SELECTARG1,"texture stage state");
    hr(device,102,{0,1}); hr(device,103,{0,scalar}); require(word(scalar)==1,"stream frequency");
    hr(device,89,{D3DFVF_XYZRHW|D3DFVF_DIFFUSE}); hr(device,90,{scalar}); require(word(scalar)==(D3DFVF_XYZRHW|D3DFVF_DIFFUSE),"FVF");
    const std::array<float,8> floats{0.5f,-1,2,4,5,6,7,8};
    const std::array<int,8> ints{-1,2,3,4,5,6,7,8};
    const std::array<BOOL,2> bools{TRUE,FALSE};
    for (uint32_t shader=0;shader<2;++shader) {
        const uint32_t start=shader?109:94;
        const uint32_t f=put(floats),i=put(ints),b=put(bools);
        const uint32_t read=put(std::array<uint32_t,8>{});
        for (auto pair : {std::array<uint32_t,3>{start,f,32}, {start+2,i,32}, {start+4,b,8}}) {
            hr(device,pair[0],{0,pair[1],2}); hr(device,pair[0]+1,{0,read,2});
            require(std::memcmp(resolve(nullptr,read,pair[2],0),resolve(nullptr,pair[1],pair[2],0),pair[2])==0,"F/I/B shader constants");
        }
    }
    hr(device,7,{put(D3DCAPS9{})}); hr(device,8,{0,put(D3DDISPLAYMODE{})});
    const uint32_t creation=put(std::array<uint32_t,4>{}); hr(device,9,{creation}); require(word(creation+8)==0x1234,"guest creation HWND");
    hr(device,3); hr(device,5);
    hr(device,79,{bits(0)}); require(call(device,80)==bits(0),"float return bits");
    std::puts("transform/material/light/viewport/scissor/clip/state/constants F/I/B round trips passed");
}
void blocks(uint32_t device)
{
    hr(device,57,{D3DRS_ALPHABLENDENABLE,FALSE});
    const uint32_t block=make(device,59,{D3DSBT_ALL}); get_device(block,3,device);
    hr(block,4); hr(device,57,{D3DRS_ALPHABLENDENABLE,TRUE}); hr(block,5);
    const uint32_t out=put(uint32_t{0}); hr(device,58,{D3DRS_ALPHABLENDENABLE,out}); require(word(out)==FALSE,"stateblock capture/apply"); release(block);
    hr(device,60); hr(device,57,{D3DRS_CULLMODE,D3DCULL_NONE});
    const uint32_t recorded=make(device,61,{});
    hr(device,57,{D3DRS_CULLMODE,D3DCULL_CCW}); hr(recorded,5);
    hr(device,58,{D3DRS_CULLMODE,out}); require(word(out)==D3DCULL_NONE,"recorded state block"); release(recorded);
    std::puts("Create/Capture/Apply and Begin/End state blocks passed");
}
void volume(uint32_t device)
{
    const uint32_t texture=make(device,24,{4,4,4,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED},{0});
    get_device(texture,3,device);
    hr(device,65,{0,texture}); const uint32_t bound=make(device,64,{0}); require(bound==texture,"GetTexture(volume) identity"); release(bound); hr(device,65,{0,0});
    require(call(texture,10)==D3DRTYPE_VOLUMETEXTURE,"volume texture resource type");
    const uint32_t level=make(texture,18,{0}); get_device(level,3,device);
    const uint32_t again=make(texture,18,{0}); require(level==again,"volume identity"); release(again);
    const GUID iid={0x2518526c,0xe789,0x4111,{0xa7,0xb9,0x47,0xef,0x32,0x8d,0x13,0xe6}};
    const uint32_t container=make(level,7,{put(iid)}); require(container==texture,"volume container identity"); release(container);
    const uint32_t desc=put(D3DVOLUME_DESC{}); hr(texture,17,{0,desc}); require(get<D3DVOLUME_DESC>(desc).Depth==4,"volume desc"); hr(level,8,{desc});
    const uint32_t out=put(LockedBox{}); hr(texture,19,{0,out,0,0}); auto lock=get<LockedBox>(out);
    for (uint32_t z=0;z<4;++z) for(uint32_t y=0;y<4;++y) for(uint32_t x=0;x<4;++x) {
        const uint32_t color=0xff000000|(z<<16)|(y<<8)|x;
        std::memcpy(resolve(nullptr,lock.bits+z*lock.slice+y*lock.row+x*4,4,0),&color,4);
    }
    const uint32_t staging=lock.bits; hr(texture,20,{0}); require(!allocations.count(staging),"volume shadow freed");
    const D3DBOX box{1,1,3,3,1,3}; const uint32_t region=put(box);
    hr(level,9,{out,region,D3DLOCK_READONLY}); lock=get<LockedBox>(out);
    for(uint32_t z=0;z<2;++z) for(uint32_t y=0;y<2;++y) for(uint32_t x=0;x<2;++x)
        require(word(lock.bits+z*lock.slice+y*lock.row+x*4)==(0xff000000|((z+1)<<16)|((y+1)<<8)|(x+1)),"volume subbox bytes");
    std::memset(resolve(nullptr,lock.bits,4,0),0,4); hr(level,10);
    hr(texture,19,{0,out,region,D3DLOCK_READONLY}); lock=get<LockedBox>(out); require(word(lock.bits)==0xff010101,"readonly volume shadow not written back"); hr(texture,20,{0});
    hr(texture,21,{region}); release(level); release(texture);
    std::puts("volume texture/volume lock, subbox, readonly, container and identity passed");
}
uint32_t compile_shader(bool vertex, const char *profile, uint32_t *size)
{
    const char *source=vertex ? "float4 main(float4 p:POSITION):POSITION { return p; }"
        : "float4 main():COLOR0 { return float4(1,0,0,1); }";
    HMODULE module=LoadLibraryW(L"d3dcompiler_47.dll"); require(module!=nullptr,"D3DCompile library");
    const FARPROC symbol=GetProcAddress(module,"D3DCompile"); pD3DCompile compile=nullptr;
    static_assert(sizeof(symbol)==sizeof(compile),"function pointer ABI"); std::memcpy(&compile,&symbol,sizeof(compile));
    require(compile!=nullptr,"D3DCompile export");
    ID3DBlob *blob=nullptr,*errors=nullptr;
    const HRESULT status=compile(source,std::strlen(source),nullptr,nullptr,nullptr,"main",profile,0,0,&blob,&errors);
    if(errors) errors->Release();
    if(FAILED(status) || !blob) { FreeLibrary(module); require(false,"D3DCompile shader"); }
    *size=static_cast<uint32_t>(blob->GetBufferSize());
    const uint32_t address=allocate(nullptr,*size,4);
    std::memcpy(resolve(nullptr,address,*size,0),blob->GetBufferPointer(),*size);
    blob->Release(); FreeLibrary(module); return address;
}
void resources(uint32_t device)
{
    const uint32_t texture=make(device,23,{8,8,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED},{0}); get_device(texture,3,device);
    hr(device,65,{0,texture}); const uint32_t got=make(device,64,{0}); require(got==texture,"GetTexture identity"); release(got); hr(device,65,{0,0});
    const uint32_t surface=make(texture,18,{0}),again=make(texture,18,{0}); require(surface==again,"surface level identity"); release(again);
    const GUID iid={0x85c31227,0x3de5,0x4f00,{0x9b,0x3a,0xf1,0x1a,0xc3,0x8c,0x18,0xb5}};
    const uint32_t container=make(surface,11,{put(iid)}); require(container==texture,"surface container identity"); release(container);
    const uint32_t out=put(LockedRect{}),rect=put(RECT{2,2,6,6}); hr(surface,13,{out,rect,0});
    auto locked=get<LockedRect>(out); const uint32_t color=0xff123456;
    std::memcpy(resolve(nullptr,locked.bits,4,0),&color,4); hr(surface,14);
    hr(texture,19,{0,out,rect,D3DLOCK_READONLY}); locked=get<LockedRect>(out); require(word(locked.bits)==color,"surface/texture shared bytes"); hr(texture,20,{0});
    hr(texture,21,{rect}); call(texture,7,{4}); require(call(texture,8)==4,"priority"); call(texture,9);
    call(texture,11,{0}); require(call(texture,12)==0 && call(texture,13)==1,"LOD and mip count"); call(texture,15); call(texture,16);
    // Raw and IUnknown private data use different host sizes; both must round trip.
    const GUID tag={0x19283746,0x1020,0x3040,{1,2,3,4,5,6,7,8}};
    const uint32_t guid=put(tag),data=put(std::array<uint32_t,2>{0x12345678,0xabcdef01}),size=put(uint32_t{8}),read=put(std::array<uint32_t,2>{});
    hr(surface,4,{guid,data,8,0}); hr(surface,5,{guid,read,size}); require(word(read)==0x12345678 && word(read+4)==0xabcdef01 && word(size)==8,"raw private data"); hr(surface,6,{guid});
    hr(surface,4,{guid,device,4,D3DSPD_IUNKNOWN});
    const uint32_t four=4; std::memcpy(resolve(nullptr,size,4,0),&four,4);
    hr(surface,5,{guid,read,size}); require(word(read)==device && word(size)==4,"IUnknown private data translated to guest proxy"); release(word(read)); hr(surface,6,{guid});
    const uint32_t bad_dc[]={surface,read}; uint32_t eax=0xdeadbeef;
    require(ac1_d3d9_dispatch(token(surface,15),bad_dc,2,&eax)==-2 && eax==0xdeadbeef && std::strstr(ac1_d3d9_last_error(),"HDC"),"precise unsupported HDC diagnostic");
    release(surface); release(texture);
    const uint32_t cube=make(device,25,{4,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED},{0}); get_device(cube,3,device);
    hr(device,65,{0,cube}); const uint32_t bound_cube=make(device,64,{0}); require(bound_cube==cube,"GetTexture(cube) identity"); release(bound_cube); hr(device,65,{0,0});
    const uint32_t face=make(cube,18,{D3DCUBEMAP_FACE_POSITIVE_X,0}),face2=make(cube,18,{D3DCUBEMAP_FACE_POSITIVE_X,0}); require(face==face2,"cube face identity"); release(face2); release(face); release(cube);
    const uint32_t compressed=make(device,23,{8,8,1,0,D3DFMT_DXT1,D3DPOOL_MANAGED},{0}); hr(compressed,19,{0,out,0,0}); locked=get<LockedRect>(out); std::memset(resolve(nullptr,locked.bits,8,0),0x5a,8); hr(compressed,20,{0}); hr(compressed,19,{0,out,0,D3DLOCK_READONLY}); locked=get<LockedRect>(out); require(get<uint8_t>(locked.bits)==0x5a,"DXT block staging"); hr(compressed,20,{0}); release(compressed);
    const uint32_t vb=make(device,26,{128,D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY,0,D3DPOOL_DEFAULT},{0}); get_device(vb,3,device);
    hr(vb,13,{put(D3DVERTEXBUFFER_DESC{})}); hr(device,100,{0,vb,8,16});
    const uint32_t offset=put(uint32_t{0}),stride=put(uint32_t{0}),buffer=put(uint32_t{0}); hr(device,101,{0,buffer,offset,stride}); require(word(buffer)==vb && word(offset)==8 && word(stride)==16,"GetStreamSource"); release(word(buffer)); hr(device,100,{0,0,0,0}); release(vb);
    const uint32_t readable=make(device,26,{64,0,0,D3DPOOL_MANAGED},{0});
    const uint32_t lock_out=put(uint32_t{0}); hr(readable,11,{0,64,lock_out,D3DLOCK_NOSYSLOCK});
    const uint32_t marker=0x19283746; std::memcpy(resolve(nullptr,word(lock_out),4,0),&marker,4); hr(readable,12);
    hr(readable,11,{0,64,lock_out,D3DLOCK_READONLY|D3DLOCK_NOSYSLOCK}); require(word(word(lock_out))==marker,"combined buffer lock flags");
    std::memset(resolve(nullptr,word(lock_out),4,0),0,4); hr(readable,12);
    hr(readable,11,{0,64,lock_out,D3DLOCK_READONLY}); require(word(word(lock_out))==marker,"combined READONLY did not write back"); hr(readable,12); release(readable);
    const uint32_t ib=make(device,27,{128,D3DUSAGE_WRITEONLY,D3DFMT_INDEX16,D3DPOOL_DEFAULT},{0}); get_device(ib,3,device); hr(ib,13,{put(D3DINDEXBUFFER_DESC{})}); hr(device,104,{ib}); const uint32_t index=make(device,105,{}); require(index==ib,"GetIndices identity"); release(index); hr(device,104,{0}); release(ib);
    const D3DVERTEXELEMENT9 elements[]={{0,0,D3DDECLTYPE_FLOAT3,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},D3DDECL_END()};
    const uint32_t decl=make(device,86,{put(elements)}); get_device(decl,3,device); hr(device,87,{decl}); const uint32_t decl2=make(device,88,{}); require(decl2==decl,"GetVertexDeclaration identity"); release(decl2); hr(device,87,{0}); release(decl);
    // Valid compiled bytecode exercises bounded length derivation for SM2/SM3,
    // GetFunction, and getter identity without passing malformed shaders native.
    for (uint32_t major=2;major<=3;++major) for (bool vertex : {true,false}) {
        uint32_t code_size=0;
        const uint32_t code_in=compile_shader(vertex,vertex?(major==2?"vs_2_0":"vs_3_0"):(major==2?"ps_2_0":"ps_3_0"),&code_size);
        const uint32_t create=vertex?91:106,bind=vertex?92:107;
        const uint32_t shader=make(device,create,{code_in}); get_device(shader,3,device); hr(device,bind,{shader});
        const uint32_t read_shader=make(device,bind+1,{}); require(read_shader==shader,"GetShader identity"); release(read_shader);
        const uint32_t length=put(uint32_t{0}); hr(shader,4,{0,length}); require(word(length)==code_size,"shader bytecode length");
        const uint32_t code=allocate(nullptr,code_size,4); hr(shader,4,{code,length}); require(std::memcmp(resolve(nullptr,code,code_size,0),resolve(nullptr,code_in,code_size,0),code_size)==0,"shader bytecode readback");
        hr(device,bind,{0}); release(shader);
    }
    const uint32_t query=make(device,118,{D3DQUERYTYPE_TIMESTAMPFREQ}); get_device(query,3,device); require(call(query,5)==8,"64-bit query result size"); hr(query,6,{D3DISSUE_END});
    const uint32_t answer=put(uint64_t{0}); const uint32_t query_result=call(query,7,{answer,8,D3DGETDATA_FLUSH}); require(query_result==S_OK || query_result==S_FALSE,"timestamp frequency query"); release(query);
    std::puts("resource inherited slots, private data, texture/surface/cube/buffer/shader/query getters passed");
}
void frame_and_reset(uint32_t d3d,uint32_t device,uint32_t pp)
{
    const uint32_t parent=make(device,6,{}); require(parent==d3d,"GetDirect3D identity"); release(parent);
    const uint32_t target=make(device,38,{0}),target2=make(device,38,{0}); require(target==target2,"GetRenderTarget identity twice");
    require(call(target,2)==1 && call(target,1)==2 && call(target,2)==1,"surface reference counts"); get_device(target,3,device);
    const uint32_t swap=make(device,14,{0}); get_device(swap,8,device);
    const uint32_t back=make(device,18,{0,0,D3DBACKBUFFER_TYPE_MONO}),swap_back=make(swap,5,{0,D3DBACKBUFFER_TYPE_MONO}); require(back==target && swap_back==target,"device/swap/backbuffer identity");
    const uint32_t present=put(Present{}); hr(swap,9,{present}); require(get<Present>(present).hwnd==0x1234,"presentation HWND marshalling");
    Present extra_parameters{}; extra_parameters.hwnd=0; extra_parameters.auto_depth=0;
    const uint32_t additional=make(device,13,{put(extra_parameters)});
    get_device(additional,8,device); hr(additional,9,{present}); require(get<Present>(present).hwnd==0x1234,"additional swapchain default HWND mapping");
    // GetNumberOfSwapChains counts implicit chains (one on this adapter).
    require(call(device,15)==1,"implicit swapchain count"); release(additional);
    hr(swap,7,{put(D3DDISPLAYMODE{})}); hr(swap,6,{put(D3DRASTER_STATUS{})});
    const uint32_t depth=make(device,40,{}); get_device(depth,3,device); release(depth);
    hr(device,43,{0,0,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0xff102030,bits(1),0});
    const uint32_t rt=make(device,28,{320,240,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE},{0});
    hr(device,34,{target,0,rt,0,D3DTEXF_NONE});
    const uint32_t system=make(device,36,{320,240,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM},{0}); hr(device,32,{rt,system});
    const uint32_t locked=put(LockedRect{}); hr(system,13,{locked,0,D3DLOCK_READONLY}); require(word(get<LockedRect>(locked).bits)==0xff102030,"Clear/StretchRect actual pixel readback"); hr(system,14);
    const uint32_t upload=make(device,36,{320,240,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT},{0}); hr(device,30,{system,0,upload,0}); release(upload); release(system); release(rt);
    const uint32_t src=make(device,23,{4,4,1,0,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM},{0}),dst=make(device,23,{4,4,1,0,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT},{0}); hr(device,31,{src,dst}); release(src); release(dst);
    struct Vertex { float x,y,z,rhw; uint32_t color; };
    const Vertex vertices[]={{20,20,0.5f,1,0xffff0000},{20,180,0.5f,1,0xff00ff00},{250,180,0.5f,1,0xff0000ff}};
    const uint32_t data=put(vertices),indices=put(std::array<uint16_t,3>{0,1,2});
    hr(device,89,{D3DFVF_XYZRHW|D3DFVF_DIFFUSE}); hr(device,57,{D3DRS_LIGHTING,FALSE}); hr(device,57,{D3DRS_CULLMODE,D3DCULL_NONE});
    hr(device,67,{0,D3DTSS_COLOROP,D3DTOP_SELECTARG1}); hr(device,67,{0,D3DTSS_COLORARG1,D3DTA_DIFFUSE});
    hr(device,41); hr(device,83,{D3DPT_TRIANGLELIST,1,data,sizeof(Vertex)}); hr(device,84,{D3DPT_TRIANGLELIST,0,3,1,indices,D3DFMT_INDEX16,data,sizeof(Vertex)}); hr(device,42);
    hr(device,17,{0,0,0,0}); hr(swap,3,{0,0,0,0,0});
    release(swap_back); release(back); release(target); release(swap);
    hr(device,16,{pp}); hr(device,3); hr(device,43,{0,0,D3DCLEAR_TARGET,0xff204060,bits(1),0}); hr(device,17,{0,0,0,0});
    // Reset must preserve the CreateDevice fullscreen-to-windowed policy and
    // expose the guest HWND and guest fullscreen fields on subsequent reads.
    Present fullscreen=get<Present>(pp); fullscreen.windowed=0; fullscreen.refresh=60;
    const uint32_t forced=put(fullscreen); require(_putenv_s("AC1_D3D9_FORCE_WINDOWED","1")==0,"force-windowed environment");
    hr(device,16,{forced}); require(get<Present>(forced).windowed==0 && get<Present>(forced).refresh==60,"Reset fullscreen guest writeback");
    const uint32_t forced_swap=make(device,14,{0}); hr(forced_swap,9,{present});
    require(get<Present>(present).windowed==0 && get<Present>(present).refresh==60 && get<Present>(present).hwnd==0x1234,"Reset/GetPresentParameters mapping"); release(forced_swap);
    require(_putenv_s("AC1_D3D9_FORCE_WINDOWED","")==0,"clear force-windowed environment"); hr(device,16,{pp});
    std::puts("Clear pixel readback, StretchRect/updates, UP draws, Present, swapchains and Reset passed");
}
} // namespace

int main(int argc,char **argv)
{
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try {
        if (argc==3 && std::strcmp(argv[1],"--provider")==0)
            require(SetEnvironmentVariableA("AC1_D3D9_LIBRARY",argv[2])!=0,"provider environment");
        else require(argc==1,"usage: bridge_coverage [--provider absolute-d3d9.dll]");
        check_arities();
        WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"AC1_D3D9_Coverage";
        require(RegisterClassW(&wc)!=0,"hidden window class");
        window=CreateWindowExW(0,wc.lpszClassName,L"AC1 bridge coverage",WS_OVERLAPPEDWINDOW,0,0,320,240,nullptr,nullptr,wc.hInstance,nullptr);
        require(window!=nullptr,"hidden window");
        ac1_d3d9_callbacks callbacks{}; callbacks.struct_size=sizeof(callbacks); callbacks.guest_resolve=resolve; callbacks.guest_alloc=allocate; callbacks.guest_free=free_memory; callbacks.guest_hwnd_to_host=hwnd; callbacks.log=log;
        require(ac1_d3d9_configure(&callbacks,nullptr)!=0,"bridge configure");
        const uint32_t d3d=ac1_d3d9_create9(D3D_SDK_VERSION); require(d3d!=0,"real provider factory");
        const uint32_t pp=put(Present{}),device=make(d3d,16,{0,D3DDEVTYPE_HAL,0x1234,D3DCREATE_HARDWARE_VERTEXPROCESSING,pp});
        states(device); blocks(device); volume(device); resources(device); frame_and_reset(d3d,device,pp);
        require(call(device,2)==0 && call(d3d,2)==0,"device/factory guest reference balance");
        ac1_d3d9_shutdown(); DestroyWindow(window); window=nullptr;
        std::puts("real-device D3D9 COM coverage passed (no fake-device fallback)"); return 0;
    } catch(const std::exception &error) {
        std::fprintf(stderr,"coverage failed: %s\n",error.what()); ac1_d3d9_shutdown();
        if(window) DestroyWindow(window);
        return 1;
    }
}
