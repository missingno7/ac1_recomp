/*
 * AC1 guest-to-host D3D9 COM ABI bridge.
 *
 * Guest object/vtable tokens follow the general COM-token technique in
 * codepdbh/nfsu2-recompiled's Android D3D9 bridge (MIT; copyright codepdbh).
 * This implementation is a separate AC1 adapter: no NFS engine code, guest
 * addresses, graphics policy, or game-specific compatibility behavior is used.
 */

#include "ac1_d3d9_bridge.h"
#ifdef AC1_PROFILE_HOOKS
#include "../runtime/ac1_profile.h"
#endif

#include <d3d9.h>
#if defined(_WIN32)
#include <windows.h>
#include <d3dx9shader.h>
#else
#include "ac1_d3dx_portable.h"
using ID3DXConstantTable = ac1_d3dx::Table;
using D3DXCONSTANTTABLE_DESC = ac1_d3dx::TableDesc;
using D3DXCONSTANT_DESC = ac1_d3dx::ConstantDesc;
using D3DXVECTOR4 = ac1_d3dx::Vector;
using D3DXMATRIX = ac1_d3dx::Matrix;
#endif

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace {

constexpr uint32_t kTokenBase = 0x7e000000u;
constexpr uint32_t kRdhRectangles = 1u;
constexpr uint32_t kKindStride = 0x1000u;
constexpr uint32_t kSlotStride = 0x10u;
constexpr uint32_t kD3D9Kind = 1u;
constexpr uint32_t kDeviceKind = 2u;
constexpr uint32_t kSurfaceKind = 3u;
constexpr uint32_t kVertexBufferKind = 4u;
constexpr uint32_t kIndexBufferKind = 5u;
constexpr uint32_t kVertexDeclarationKind = 6u;
constexpr uint32_t kTextureKind = 7u;
constexpr uint32_t kPixelShaderKind = 8u;
constexpr uint32_t kVertexShaderKind = 9u;
constexpr uint32_t kConstantTableKind = 10u;
constexpr uint32_t kCubeTextureKind = 11u;
constexpr uint32_t kQueryKind = 12u;
constexpr uint32_t kVolumeTextureKind = 13u;
constexpr uint32_t kVolumeKind = 14u;
constexpr uint32_t kStateBlockKind = 15u;
constexpr uint32_t kSwapChainKind = 16u;
constexpr uint32_t kD3D9VtableSlots = 17u;
constexpr uint32_t kDeviceVtableSlots = 119u;
constexpr uint32_t kSurfaceVtableSlots = 17u;
constexpr uint32_t kPixelShaderVtableSlots = 5u;
constexpr uint32_t kVertexShaderVtableSlots = 5u;
constexpr uint32_t kConstantTableVtableSlots = 27u;
constexpr uint32_t kCubeTextureVtableSlots = 22u;
constexpr uint32_t kQueryVtableSlots = 8u;

constexpr GUID kIidIUnknown =
    {0x00000000u, 0x0000u, 0x0000u, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIidIDirect3D9 =
    {0x81bdcbcau, 0x64d4u, 0x426du, {0xae, 0x8d, 0xad, 0x01, 0x47, 0xf4, 0x27, 0x5c}};
constexpr GUID kIidIDirect3DDevice9 =
    {0xd0223b96u, 0xbf7au, 0x43fdu, {0x92, 0xbd, 0xa4, 0x3b, 0x0d, 0x82, 0xb9, 0xeb}};
constexpr GUID kIidIDirect3DQuery9 =
    {0xd9771460u, 0xa695u, 0x4f26u, {0xbb, 0xd3, 0x27, 0xb8, 0x40, 0xb5, 0x41, 0xcc}};
constexpr GUID kIidIDirect3DSurface9 =
    {0x0cfbaf3au, 0x9ff6u, 0x429au, {0x99, 0xb3, 0xa2, 0x79, 0x6a, 0xf8, 0xb8, 0x9b}};
constexpr GUID kIidIDirect3DVertexBuffer9 =
    {0xb64bb1b5u, 0xfd70u, 0x4df6u, {0xbf, 0x91, 0x19, 0xd0, 0xa1, 0x24, 0x55, 0xe3}};
constexpr GUID kIidIDirect3DIndexBuffer9 =
    {0x7c9dd65eu, 0xd3f7u, 0x4529u, {0xac, 0xee, 0x78, 0x58, 0x30, 0xac, 0xde, 0x35}};
constexpr GUID kIidIDirect3DVertexDeclaration9 =
    {0xdd13c59cu, 0x36fau, 0x4098u, {0xa8, 0xfb, 0xc7, 0xed, 0x39, 0xdc, 0x85, 0x46}};
constexpr GUID kIidIDirect3DBaseTexture9 =
    {0x580ca87eu, 0x1d3cu, 0x4d54u, {0x99, 0x1d, 0xb7, 0xd3, 0xe3, 0xc2, 0x98, 0xce}};
constexpr GUID kIidIDirect3DTexture9 =
    {0x85c31227u, 0x3de5u, 0x4f00u, {0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5}};
constexpr GUID kIidIDirect3DCubeTexture9 =
    {0xfff32f81u, 0xd953u, 0x473au, {0x92, 0x23, 0x93, 0xd6, 0x52, 0xab, 0xa9, 0x3f}};
constexpr GUID kIidIDirect3DPixelShader9 =
    {0x6d3bdbdcu, 0x5b02u, 0x4415u, {0xb8, 0x52, 0xce, 0x5e, 0x8b, 0xcc, 0xb2, 0x89}};
constexpr GUID kIidIDirect3DVertexShader9 =
    {0xefc5557eu, 0x6265u, 0x4613u, {0x8a, 0x94, 0x43, 0x85, 0x78, 0x89, 0xeb, 0x36}};
/* D3DX9_36 predates the SDK 43 ConstantTable IID. */
constexpr GUID kIidID3DXConstantTable36 =
    {0x9dca3190u, 0x38b9u, 0x4fc3u, {0x92, 0xe3, 0x39, 0xc6, 0xdd, 0xfb, 0x35, 0x8b}};

constexpr GUID kIidIDirect3DResource9 = {0x05eec05du,0x8f7du,0x4362u,{0xb9,0x99,0xd1,0xba,0xf3,0x57,0xc7,0x04}};
constexpr GUID kIidIDirect3DVolumeTexture9 = {0x2518526cu,0xe789u,0x4111u,{0xa7,0xb9,0x47,0xef,0x32,0x8d,0x13,0xe6}};
constexpr GUID kIidIDirect3DVolume9 = {0x24f416e6u,0x1f67u,0x4aa7u,{0xb8,0x8e,0xd3,0x3f,0x6f,0x31,0x28,0xa1}};
constexpr GUID kIidIDirect3DStateBlock9 = {0xb07c4fe5u,0x310du,0x4ba8u,{0xa2,0x3c,0x4f,0x0f,0x20,0x6f,0x21,0x8b}};
constexpr GUID kIidIDirect3DSwapChain9 = {0x794950f2u,0xadfcu,0x458au,{0x90,0x5e,0x10,0xa1,0x0b,0x0b,0x50,0x3b}};

enum class ObjectKind : uint32_t {
    D3D9 = kD3D9Kind,
    Device = kDeviceKind,
    Surface = kSurfaceKind,
    VertexBuffer = kVertexBufferKind,
    IndexBuffer = kIndexBufferKind,
    VertexDeclaration = kVertexDeclarationKind,
    Texture = kTextureKind,
    PixelShader = kPixelShaderKind,
    VertexShader = kVertexShaderKind,
    ConstantTable = kConstantTableKind,
    CubeTexture = kCubeTextureKind,
    Query = kQueryKind,
    VolumeTexture = kVolumeTextureKind,
    Volume = kVolumeKind,
    StateBlock = kStateBlockKind,
    SwapChain = kSwapChainKind
};

struct GuestObject {
    IUnknown *native = nullptr;
    ObjectKind kind = ObjectKind::D3D9;
    uint32_t guest_vtable = 0;
    uint32_t guest_references = 1;
    uint32_t guest_focus = 0;
    uint32_t guest_window = 0;
    bool forced_windowed = false;
    uint32_t fullscreen_refresh = 0;
    uint32_t buffer_lock_guest_address = 0;
    uint32_t buffer_lock_offset = 0;
    uint32_t buffer_lock_size = 0;
    uint32_t buffer_lock_flags = 0;
    void *buffer_lock_native_pointer = nullptr;
    /* Persistent guest copy of a vertex/index buffer (see lock_guest_buffer). */
    uint32_t buffer_shadow = 0;
    uint32_t buffer_shadow_size = 0;
    bool buffer_lock_shadowed = false;
};

#pragma pack(push, 4)
struct GuestLockedRect {
    int32_t pitch;
    uint32_t bits;
};
struct GuestLockedBox {
    int32_t row_pitch;
    int32_t slice_pitch;
    uint32_t bits;
};
#pragma pack(pop)
static_assert(sizeof(GuestLockedBox) == 12, "guest D3DLOCKED_BOX must match PE32 ABI");
static_assert(sizeof(GuestLockedRect) == 8,
              "guest D3DLOCKED_RECT must match PE32 ABI");

#pragma pack(push, 4)
struct GuestD3DXConstantTableDesc {
    uint32_t creator;
    uint32_t version;
    uint32_t constants;
};
struct GuestD3DXConstantDesc {
    uint32_t name;
    uint32_t register_set;
    uint32_t register_index;
    uint32_t register_count;
    uint32_t parameter_class;
    uint32_t parameter_type;
    uint32_t rows;
    uint32_t columns;
    uint32_t elements;
    uint32_t struct_members;
    uint32_t bytes;
    uint32_t default_value;
};
#pragma pack(pop)
static_assert(sizeof(GuestD3DXConstantTableDesc) == 12,
              "guest D3DXCONSTANTTABLE_DESC must match PE32 ABI");
static_assert(sizeof(GuestD3DXConstantDesc) == 48,
              "guest D3DXCONSTANT_DESC must match PE32 ABI");

struct TextureLockKey {
    uint32_t guest_texture;
    uint32_t face;
    uint32_t level;

    bool operator<(const TextureLockKey &other) const
    {
        if (guest_texture != other.guest_texture)
            return guest_texture < other.guest_texture;
        if (face != other.face) return face < other.face;
        return level < other.level;
    }
};

/* D3DPRESENT_PARAMETERS is 56 bytes in the 32-bit guest ABI. HWND is the only
 * pointer-sized member; keep it explicitly 32-bit and translate it at the
 * host boundary instead of copying the 64-bit host structure. */
#pragma pack(push, 4)
struct GuestPresentParameters {
    uint32_t back_buffer_width;
    uint32_t back_buffer_height;
    uint32_t back_buffer_format;
    uint32_t back_buffer_count;
    uint32_t multi_sample_type;
    uint32_t multi_sample_quality;
    uint32_t swap_effect;
    uint32_t device_window;
    int32_t windowed;
    int32_t enable_auto_depth_stencil;
    uint32_t auto_depth_stencil_format;
    uint32_t flags;
    uint32_t fullscreen_refresh_rate;
    uint32_t presentation_interval;
};
#pragma pack(pop)
#pragma pack(push, 8)
struct GuestAdapterIdentifier {
    char driver[MAX_DEVICE_IDENTIFIER_STRING];
    char description[MAX_DEVICE_IDENTIFIER_STRING];
    char device_name[32];
    int64_t driver_version;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t subsystem_id;
    uint32_t revision;
    GUID device_identifier;
    uint32_t whql_level;
};
#pragma pack(pop)
static_assert(sizeof(GuestPresentParameters) == 56,
              "guest D3DPRESENT_PARAMETERS must match PE32 ABI");
static_assert(sizeof(D3DDISPLAYMODE) == 16,
              "D3DDISPLAYMODE is architecture independent");
static_assert(sizeof(GuestAdapterIdentifier) == 1104,
              "guest D3DADAPTER_IDENTIFIER9 layout mismatch");
static_assert(offsetof(GuestAdapterIdentifier, driver_version) == 1056,
              "guest D3DADAPTER_IDENTIFIER9 driver-version offset mismatch");
static_assert(sizeof(D3DADAPTER_IDENTIFIER9) == 1104,
              "D3DADAPTER_IDENTIFIER9 guest/host ABI mismatch");
static_assert(sizeof(D3DCAPS9) == 304,
              "D3DCAPS9 guest/host ABI mismatch");
static_assert(sizeof(D3DSURFACE_DESC) == 32,
              "D3DSURFACE_DESC guest/host ABI mismatch");
static_assert(sizeof(D3DMATRIX) == 64 && sizeof(D3DMATERIAL9) == 68 &&
              sizeof(D3DLIGHT9) == 104 && sizeof(D3DVOLUME_DESC) == 28 &&
              sizeof(D3DBOX) == 24 && sizeof(RECT) == 16,
              "D3D9 structure sizes must match the guest ABI");
static_assert(sizeof(D3DVERTEXELEMENT9) == 8,
              "guest D3DVERTEXELEMENT9 must remain 8 bytes");

ac1_d3d9_callbacks g_callbacks{};
void *g_context = nullptr;
std::map<uint32_t, GuestObject> g_objects;
std::map<IUnknown *, uint32_t> g_guest_by_native;
struct GuestConstantHandle {
    uint32_t table = 0;
    const char *native = nullptr;
};
std::map<uint32_t, std::vector<uint32_t>> g_constant_table_allocations;
std::map<uint32_t, GuestConstantHandle> g_constant_handles;
std::map<std::pair<uint32_t, uintptr_t>, uint32_t> g_guest_handle_by_native;
std::mutex g_mutex;
uint64_t g_successful_draw_calls = 0;
std::atomic<uint64_t> g_successful_present_calls{0};
bool g_frame_trace_enabled = false;
bool g_frame_trace_initialized = false;
bool g_first_draw_reported = false;
bool g_first_present_reported = false;
/* MinGW thread-exit crashes were reproduced with nontrivial TLS destructors. */
thread_local char g_last_error[1024]{};
std::string g_d3d9_provider_path;
#if defined(_WIN32)
HMODULE g_d3d9_module = nullptr;
HMODULE g_d3dx9_module = nullptr;
using D3DXCheckTextureRequirementsFn = HRESULT (WINAPI *)(
    IDirect3DDevice9 *, UINT *, UINT *, UINT *, DWORD, D3DFORMAT *, D3DPOOL);
D3DXCheckTextureRequirementsFn g_d3dx_check_texture_requirements = nullptr;
using D3DXGetShaderConstantTableFn = HRESULT (WINAPI *)(
    const DWORD *, ID3DXConstantTable **);
D3DXGetShaderConstantTableFn g_d3dx_get_shader_constant_table = nullptr;
#endif

#if defined(_WIN32)
/* Evidence capture: with AC1_D3D9_CAPTURE_DIR set, write the current back
 * buffer as a 32-bit BMP before Presents 1, 2, 4, 8, ... (power-of-two count
 * of the upcoming present). Failures are reported and never alter Present. */
uint64_t g_capture_present_index = 0;
void capture_back_buffer(IDirect3DDevice9 *device)
{
    const char *dir = std::getenv("AC1_D3D9_CAPTURE_DIR");
    if (!dir || !*dir || !device) return;
    const uint64_t n = ++g_capture_present_index;
    const char *every = std::getenv("AC1_D3D9_CAPTURE_EVERY");
    const uint64_t interval = every ? std::strtoull(every, nullptr, 10) : 0;
    if ((n & (n - 1u)) && (!interval || n % interval)) return;
    IDirect3DSurface9 *back = nullptr, *copy = nullptr;
    D3DSURFACE_DESC desc{};
    D3DLOCKED_RECT locked{};
    HRESULT hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
    if (SUCCEEDED(hr)) hr = back->GetDesc(&desc);
    if (SUCCEEDED(hr) && desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8)
        hr = E_NOTIMPL;
    if (SUCCEEDED(hr))
        hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                 D3DPOOL_SYSTEMMEM, &copy, nullptr);
    if (SUCCEEDED(hr)) hr = device->GetRenderTargetData(back, copy);
    if (SUCCEEDED(hr)) hr = copy->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    if (SUCCEEDED(hr)) {
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/present_%06llu.bmp", dir,
                      static_cast<unsigned long long>(n));
        FILE *file = std::fopen(path, "wb");
        if (file) {
            const uint32_t row = desc.Width * 4u, image = row * desc.Height;
            uint8_t header[54]{};
            auto put32 = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
            header[0] = 'B'; header[1] = 'M';
            put32(2, 54u + image); put32(10, 54u); put32(14, 40u);
            put32(18, desc.Width); put32(22, static_cast<uint32_t>(-static_cast<int32_t>(desc.Height)));
            header[26] = 1; header[28] = 32; put32(34, image);
            std::fwrite(header, 1, sizeof(header), file);
            for (uint32_t y = 0; y < desc.Height; ++y)
                std::fwrite(static_cast<const uint8_t *>(locked.pBits) + y * locked.Pitch, 1, row, file);
            std::fclose(file);
            std::fprintf(stderr, "AC1 D3D9 capture: %s (%ux%u format=%u draw_calls=%llu)\n",
                         path, desc.Width, desc.Height, static_cast<unsigned>(desc.Format),
                         static_cast<unsigned long long>(g_successful_draw_calls));
        } else {
            std::fprintf(stderr, "AC1 D3D9 capture: cannot open %s\n", path);
        }
        copy->UnlockRect();
    } else {
        std::fprintf(stderr, "AC1 D3D9 capture: present %llu failed hr=0x%08lx\n",
                     static_cast<unsigned long long>(n), static_cast<unsigned long>(hr));
    }
    if (copy) copy->Release();
    if (back) back->Release();
}
#endif

void trace_frame_success(bool is_present, const char *method)
{
#ifdef AC1_PROFILE_HOOKS
    if (is_present && ac1_profile_frames_enabled) ac1_profile_present();
#endif
#if defined(_WIN32)
    if (!g_frame_trace_initialized) {
        char value[8]{};
        g_frame_trace_enabled = GetEnvironmentVariableA(
            "AC1_D3D9_TRACE_FRAME", value, static_cast<DWORD>(sizeof(value))) != 0;
        g_frame_trace_initialized = true;
    }
    if (is_present) g_successful_present_calls.fetch_add(1, std::memory_order_relaxed);
    if (!g_frame_trace_enabled) return;
    if (is_present) {
        const uint64_t n = g_successful_present_calls;
        if (n >= 2u && !(n & (n - 1u)))
            std::fprintf(stderr, "AC1 D3D9 trace: present_calls=%llu draw_calls=%llu t_ms=%lu\n",
                         static_cast<unsigned long long>(n),
                         static_cast<unsigned long long>(g_successful_draw_calls),
                         static_cast<unsigned long>(GetTickCount()));
        /* stderr is block-buffered; keep the log useful when a timed run is
         * terminated. Power-of-two presents make this rare. */
        if (n >= 2u && !(n & (n - 1u))) std::fflush(stderr);
        if (!g_first_present_reported) {
            std::fprintf(stderr,
                "AC1 D3D9 trace: first successful Present via %s (present_calls=%llu, draw_calls=%llu)\n",
                method, static_cast<unsigned long long>(g_successful_present_calls),
                static_cast<unsigned long long>(g_successful_draw_calls));
            g_first_present_reported = true;
        }
    } else {
        ++g_successful_draw_calls;
        if (!g_first_draw_reported) {
            std::fprintf(stderr,
                "AC1 D3D9 trace: first successful draw via %s (draw_calls=%llu)\n",
                method, static_cast<unsigned long long>(g_successful_draw_calls));
            g_first_draw_reported = true;
        }
    }
#else
    (void)is_present;
    (void)method;
#endif
}

void set_error(const std::string &message)
{
    const size_t length = std::min(message.size(), sizeof(g_last_error) - 1);
    std::memcpy(g_last_error, message.data(), length);
    g_last_error[length] = '\0';
    if (g_callbacks.log) g_callbacks.log(g_context, g_last_error);
#if defined(_WIN32)
    else {
        std::string line = "AC1 D3D9 bridge: " + message + "\n";
        OutputDebugStringA(line.c_str());
    }
#endif
}

void clear_error()
{
    g_last_error[0] = '\0';
}

#if defined(_WIN32)
bool is_absolute_windows_path(const std::wstring &path)
{
    const auto separator = [](wchar_t value) {
        return value == L'\\' || value == L'/';
    };
    if (path.size() >= 3 &&
        ((path[0] >= L'A' && path[0] <= L'Z') ||
         (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && separator(path[2]))
        return true;
    /* UNC and Win32 device paths are fully qualified; a single rooted slash
     * and drive-relative `C:foo` are not absolute and must be rejected. */
    return path.size() >= 3 && separator(path[0]) && separator(path[1]) &&
           !separator(path[2]);
}

std::string windows_path_utf8(const std::wstring &path)
{
    const int required = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1,
                                              nullptr, 0, nullptr, nullptr);
    if (required <= 1) return std::string();
    std::string result(static_cast<size_t>(required), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, result.data(),
                             required, nullptr, nullptr))
        return std::string();
    result.resize(static_cast<size_t>(required - 1));
    return result;
}

bool selected_d3d9_module_path(std::wstring *path, bool *explicit_path)
{
    if (!path || !explicit_path) return false;
    *explicit_path = false;
    SetLastError(ERROR_SUCCESS);
    const DWORD required = GetEnvironmentVariableW(L"AC1_D3D9_LIBRARY", nullptr, 0);
    const DWORD environment_error = GetLastError();
    if (required != 0 || environment_error != ERROR_ENVVAR_NOT_FOUND) {
        *explicit_path = true;
        if (!required) {
            set_error(environment_error == ERROR_SUCCESS
                ? "AC1_D3D9_LIBRARY is explicitly set but empty"
                : "failed to read AC1_D3D9_LIBRARY");
            return false;
        }
        std::vector<wchar_t> value(required);
        const DWORD copied = GetEnvironmentVariableW(
            L"AC1_D3D9_LIBRARY", value.data(), static_cast<DWORD>(value.size()));
        if (!copied || copied >= value.size()) {
            set_error("AC1_D3D9_LIBRARY changed while it was being read");
            return false;
        }
        path->assign(value.data(), copied);
        if (!is_absolute_windows_path(*path)) {
            set_error("AC1_D3D9_LIBRARY must be an absolute Windows DLL path: " +
                      windows_path_utf8(*path));
            return false;
        }
        return true;
    }

    wchar_t system_directory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
    if (!length || length >= MAX_PATH - 10) {
        set_error("GetSystemDirectoryW failed while locating the default system D3D9");
        return false;
    }
    path->assign(system_directory, length);
    *path += L"\\d3d9.dll";
    return true;
}

bool ensure_d3d9_module()
{
    if (g_d3d9_module) return true;
    std::wstring path;
    bool explicit_path = false;
    if (!selected_d3d9_module_path(&path, &explicit_path)) return false;
    const std::string selected_utf8 = windows_path_utf8(path);
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
        const DWORD error = GetLastError();
        std::ostringstream message;
        if (explicit_path)
            message << "LoadLibraryExW failed for AC1_D3D9_LIBRARY " << selected_utf8
                    << " (Win32 error " << error << ')';
        else
            message << "LoadLibraryExW failed for the system d3d9.dll (Win32 error "
                    << error << ')';
        set_error(message.str());
        return false;
    }
    g_d3d9_module = module;
    g_d3d9_provider_path = selected_utf8;
    std::vector<wchar_t> loaded_path(32768);
    const DWORD loaded_length = GetModuleFileNameW(
        g_d3d9_module, loaded_path.data(), static_cast<DWORD>(loaded_path.size()));
    if (loaded_length && loaded_length < loaded_path.size()) {
        g_d3d9_provider_path = windows_path_utf8(
            std::wstring(loaded_path.data(), loaded_length));
    }
    return true;
}

bool ensure_d3dx9_module()
{
    if (g_d3dx9_module && g_d3dx_check_texture_requirements) return true;
    wchar_t system_directory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
    if (!length || length >= MAX_PATH - 16) {
        set_error("GetSystemDirectoryW failed while locating d3dx9_36.dll");
        return false;
    }
    std::wstring path(system_directory, length);
    path += L"\\d3dx9_36.dll";
    g_d3dx9_module = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_d3dx9_module) {
        std::ostringstream message;
        message << "LoadLibraryExW failed for absolute System32 d3dx9_36.dll (Win32 error "
                << GetLastError() << ')';
        set_error(message.str());
        return false;
    }
    FARPROC raw_function = GetProcAddress(g_d3dx9_module,
                                           "D3DXCheckTextureRequirements");
    static_assert(sizeof(D3DXCheckTextureRequirementsFn) == sizeof(raw_function),
                  "Windows function-pointer ABI mismatch");
    std::memcpy(&g_d3dx_check_texture_requirements, &raw_function,
                sizeof(g_d3dx_check_texture_requirements));
    if (!g_d3dx_check_texture_requirements) {
        set_error("System32 d3dx9_36.dll has no D3DXCheckTextureRequirements export");
        FreeLibrary(g_d3dx9_module);
        g_d3dx9_module = nullptr;
        return false;
    }
    return true;
}

bool ensure_d3dx_shader_table_export()
{
    if (!ensure_d3dx9_module()) return false;
    if (!g_d3dx_get_shader_constant_table) {
        FARPROC raw_function = GetProcAddress(g_d3dx9_module,
                                               "D3DXGetShaderConstantTable");
        static_assert(sizeof(D3DXGetShaderConstantTableFn) == sizeof(raw_function),
                      "Windows D3DX function-pointer ABI mismatch");
        std::memcpy(&g_d3dx_get_shader_constant_table, &raw_function,
                    sizeof(g_d3dx_get_shader_constant_table));
    }
    if (!g_d3dx_get_shader_constant_table) {
        set_error("System32 d3dx9_36.dll has no D3DXGetShaderConstantTable export");
        return false;
    }
    return true;
}
#endif

void *guest_memory(uint32_t address, uint32_t width, uint32_t access)
{
    if (!address || !width || !g_callbacks.guest_resolve) return nullptr;
    return g_callbacks.guest_resolve(g_context, address, width, access);
}

bool guest_read(uint32_t address, void *out, uint32_t width)
{
    void *source = guest_memory(address, width, AC1_D3D9_GUEST_READ);
    if (!source) return false;
    std::memcpy(out, source, width);
    return true;
}

bool guest_write(uint32_t address, const void *value, uint32_t width)
{
    void *destination = guest_memory(address, width, AC1_D3D9_GUEST_WRITE);
    if (!destination) return false;
    std::memcpy(destination, value, width);
    return true;
}

uint32_t slot_token(ObjectKind kind, uint32_t slot)
{
    return kTokenBase + static_cast<uint32_t>(kind) * kKindStride +
           slot * kSlotStride;
}

uint32_t slot_count(ObjectKind kind)
{
    switch (kind) {
    case ObjectKind::D3D9: return kD3D9VtableSlots;
    case ObjectKind::Device: return kDeviceVtableSlots;
    case ObjectKind::Surface: return kSurfaceVtableSlots;
    case ObjectKind::VertexBuffer: return 14u;
    case ObjectKind::IndexBuffer: return 14u;
    case ObjectKind::VertexDeclaration: return 5u;
    case ObjectKind::Texture: return 22u;
    case ObjectKind::PixelShader: return kPixelShaderVtableSlots;
    case ObjectKind::VertexShader: return kVertexShaderVtableSlots;
    case ObjectKind::ConstantTable: return kConstantTableVtableSlots;
    case ObjectKind::CubeTexture: return kCubeTextureVtableSlots;
    case ObjectKind::Query: return kQueryVtableSlots;
    case ObjectKind::VolumeTexture: return 22u;
    case ObjectKind::Volume: return 11u;
    case ObjectKind::StateBlock: return 6u;
    case ObjectKind::SwapChain: return 10u;
    }
    return 0;
}

int token_argument_count(uint32_t token)
{
    if (token < kTokenBase || token >= kTokenBase + 17 * kKindStride) return 0;
    const uint32_t offset = token - kTokenBase;
    const uint32_t kind = offset / kKindStride;
    const uint32_t within_kind = offset % kKindStride;
    if (kind < kD3D9Kind || kind > kSwapChainKind) return 0;
    if (within_kind % kSlotStride) return -1;
    const uint32_t method = within_kind / kSlotStride;
    if (method >= slot_count(static_cast<ObjectKind>(kind))) return -1;
    static constexpr int arity1[] = {3, 1, 1, -2, 1, 4, 3, 5, 3, 6, 7, 7, 6, 5, 4, 2, 7}; // IDirect3D9
    static constexpr int arity2[] = {3, 1, 1, 1, 1, 1, 2, 2, 3, 2, 4, 4, 2, 3, 3, 1, 2, 5, 5, 3, 2, 4, 3, 9, 10, 8, 7, 7, 9, 9, 5, 3, 3, 3, 6, 4, 7, 3, 3, 2, 2, 1, 1, 7, 3, 3, 3, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1, 2, 2, 2, 3, 3, 4, 4, 4, 4, 2, 3, 3, 2, 2, 2, 2, 2, 1, 2, 1, 4, 7, 5, 9, 7, 3, 2, 2, 2, 2, 3, 2, 2, 4, 4, 4, 4, 4, 4, 5, 5, 3, 3, 2, 2, 3, 2, 2, 4, 4, 4, 4, 4, 4, 4, 4, 2, 3}; // IDirect3DDevice9
    static constexpr int arity3[] = {3, 1, 1, 2, 5, 4, 2, 2, 1, 1, 1, 3, 2, 4, 1, 2, 2}; // IDirect3DSurface9
    static constexpr int arity4[] = {3, 1, 1, 2, 5, 4, 2, 2, 1, 1, 1, 5, 1, 2}; // IDirect3DVertexBuffer9
    static constexpr int arity5[] = {3, 1, 1, 2, 5, 4, 2, 2, 1, 1, 1, 5, 1, 2}; // IDirect3DIndexBuffer9
    static constexpr int arity6[] = {3, 1, 1, 2, 3}; // IDirect3DVertexDeclaration9
    static constexpr int arity7[] = {3, 1, 1, 2, 5, 4, 2, 2, 1, 1, 1, 2, 1, 1, 2, 1, 1, 3, 3, 5, 2, 2}; // IDirect3DTexture9
    static constexpr int arity8[] = {3, 1, 1, 2, 3}; // IDirect3DPixelShader9
    static constexpr int arity9[] = {3, 1, 1, 2, 3}; // IDirect3DVertexShader9
    static constexpr int arity11[] = {3, 1, 1, 2, 5, 4, 2, 2, 1, 1, 1, 2, 1, 1, 2, 1, 1, 3, 4, 6, 3, 3}; // IDirect3DCubeTexture9
    static constexpr int arity12[] = {3, 1, 1, 2, 1, 1, 2, 4}; // IDirect3DQuery9
    static constexpr int arity13[] = {3, 1, 1, 2, 5, 4, 2, 2, 1, 1, 1, 2, 1, 1, 2, 1, 1, 3, 3, 5, 2, 2}; // IDirect3DVolumeTexture9
    static constexpr int arity14[] = {3, 1, 1, 2, 5, 4, 2, 3, 2, 4, 1}; // IDirect3DVolume9
    static constexpr int arity15[] = {3, 1, 1, 2, 1, 1}; // IDirect3DStateBlock9
    static constexpr int arity16[] = {3, 1, 1, 6, 2, 4, 2, 2, 2, 2}; // IDirect3DSwapChain9
    // ID3DXConstantTable SDK slots. Buffer pointers and matrix pointer arrays
    // have no guest ABI adapter; unsupported slots remain explicit stops.
    static constexpr int arity10[] = {3,1,1,-2,-2,2,4,2,3,3,3,2,5,4,5,4,5,4,5,4,5,4,5,-2,4,5,-2};
    const int *const tables[] = {nullptr, arity1, arity2, arity3, arity4,
        arity5, arity6, arity7, arity8, arity9, arity10, arity11, arity12,
        arity13, arity14, arity15, arity16};
    return tables[kind][method];
}

bool write_guest_word(uint32_t address, uint32_t value)
{
    return guest_write(address, &value, sizeof(value));
}

void free_guest_object(uint32_t address, const GuestObject &object)
{
    if (g_callbacks.guest_free) {
        if (object.buffer_shadow) g_callbacks.guest_free(g_context, object.buffer_shadow);
        if (object.guest_vtable) g_callbacks.guest_free(g_context, object.guest_vtable);
        if (address) g_callbacks.guest_free(g_context, address);
    }
}

uint32_t wrap_native(IUnknown *native, ObjectKind kind, bool acquired_reference)
{
    if (!native) return 0;
    auto existing = g_guest_by_native.find(native);
    if (existing != g_guest_by_native.end()) {
        auto object = g_objects.find(existing->second);
        if (object == g_objects.end() || object->second.kind != kind) {
            set_error("native COM pointer is already wrapped with a different interface kind");
            if (acquired_reference) native->Release();
            return 0;
        }
        ++object->second.guest_references;
        /* QueryInterface already acquired the host reference. */
        if (!acquired_reference) native->AddRef();
        return existing->second;
    }

    const uint32_t slots = slot_count(kind);
    const uint32_t table_bytes = slots * sizeof(uint32_t);
    uint32_t table = g_callbacks.guest_alloc(g_context, table_bytes, 4);
    if (!table) {
        set_error("guest allocation failed for D3D9 vtable");
        if (acquired_reference) native->Release();
        return 0;
    }
    uint32_t address = g_callbacks.guest_alloc(g_context, sizeof(uint32_t), 4);
    if (!address) {
        set_error("guest allocation failed for D3D9 object");
        g_callbacks.guest_free(g_context, table);
        if (acquired_reference) native->Release();
        return 0;
    }

    void *table_host = guest_memory(table, table_bytes, AC1_D3D9_GUEST_WRITE);
    if (!table_host) {
        set_error("allocated D3D9 vtable is not writable in guest memory");
        g_callbacks.guest_free(g_context, address);
        g_callbacks.guest_free(g_context, table);
        if (acquired_reference) native->Release();
        return 0;
    }
    for (uint32_t i = 0; i < slots; ++i) {
        const uint32_t token = slot_token(kind, i);
        std::memcpy(static_cast<uint8_t *>(table_host) + i * sizeof(token),
                    &token, sizeof(token));
    }
    if (!write_guest_word(address, table)) {
        set_error("allocated D3D9 guest object is not writable");
        g_callbacks.guest_free(g_context, address);
        g_callbacks.guest_free(g_context, table);
        if (acquired_reference) native->Release();
        return 0;
    }

    GuestObject object;
    object.native = native;
    object.kind = kind;
    object.guest_vtable = table;
    g_objects.emplace(address, object);
    g_guest_by_native.emplace(native, address);
    return address;
}

GuestObject *find_object(uint32_t address, ObjectKind expected)
{
    auto it = g_objects.find(address);
    if (it == g_objects.end() || it->second.kind != expected) return nullptr;
    return &it->second;
}

uint32_t hresult(HRESULT value)
{
    return static_cast<uint32_t>(value);
}

bool require_count(uint32_t got, uint32_t expected, const char *method)
{
    if (got == expected) return true;
    std::ostringstream message;
    message << method << " received " << got << " guest arguments; expected " << expected;
    set_error(message.str());
    return false;
}

enum class ShaderConstantData : uint8_t {
    VertexFloat,
    VertexInt,
    VertexBool,
    PixelFloat,
    PixelInt,
    PixelBool
};

HRESULT checked_shader_constant_data(IDirect3DDevice9 *device,
                                     ShaderConstantData kind,
                                     uint32_t start_register,
                                     uint32_t guest_data,
                                     uint32_t register_count,
                                     const void **data)
{
    if (!device || !data) return D3DERR_INVALIDCALL;
    D3DCAPS9 caps{};
    HRESULT status = device->GetDeviceCaps(&caps);
    if (FAILED(status)) {
        set_error("shader constant validation could not query device caps");
        return status;
    }

    uint32_t maximum_registers = 16u;
    switch (kind) {
    case ShaderConstantData::VertexFloat:
        maximum_registers = caps.MaxVertexShaderConst;
        break;
    case ShaderConstantData::PixelFloat: {
        const uint32_t major = (caps.PixelShaderVersion >> 8u) & 0xffu;
        maximum_registers = major >= 3u ? 224u : major == 2u ? 32u : major == 1u ? 8u : 0u;
        break;
    }
    case ShaderConstantData::VertexInt:
    case ShaderConstantData::VertexBool:
    case ShaderConstantData::PixelInt:
    case ShaderConstantData::PixelBool:
        /* D3D9 shader-model-3 integer and boolean register files are 16 wide. */
        maximum_registers = 16u;
        break;
    }
    if (start_register > maximum_registers ||
        register_count > maximum_registers - start_register) {
        set_error("shader constant register range exceeds D3D9 device limits");
        return D3DERR_INVALIDCALL;
    }

    uint32_t bytes_per_register = 16u;
    if (kind == ShaderConstantData::VertexBool ||
        kind == ShaderConstantData::PixelBool)
        bytes_per_register = sizeof(BOOL);
    const uint64_t byte_count = static_cast<uint64_t>(register_count) * bytes_per_register;
    if (byte_count > UINT32_MAX) {
        set_error("shader constant guest buffer size overflows the 32-bit guest ABI");
        return D3DERR_INVALIDCALL;
    }
    if (!register_count) {
        *data = guest_data ? guest_memory(guest_data, 1u, AC1_D3D9_GUEST_READ) : nullptr;
        if (guest_data && !*data) {
            set_error("zero-count shader constant pointer is unmapped");
            return D3DERR_INVALIDCALL;
        }
        return D3D_OK;
    }
    *data = guest_memory(guest_data, static_cast<uint32_t>(byte_count),
                         AC1_D3D9_GUEST_READ);
    if (!*data) {
        set_error("shader constant guest array is unmapped");
        return D3DERR_INVALIDCALL;
    }
    return D3D_OK;
}

bool supports_guest_buffer_lock_flags(uint32_t flags)
{
    constexpr uint32_t known = D3DLOCK_NOOVERWRITE | D3DLOCK_DISCARD |
        D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK | D3DLOCK_DONOTWAIT | D3DLOCK_NO_DIRTY_UPDATE;
    return (flags & ~known) == 0;
}

bool bridge_supports_iid(ObjectKind kind, const GUID &iid)
{
    const GUID *supported = &kIidIDirect3DSurface9;
    switch (kind) {
    case ObjectKind::D3D9: supported = &kIidIDirect3D9; break;
    case ObjectKind::Device: supported = &kIidIDirect3DDevice9; break;
    case ObjectKind::Surface: supported = &kIidIDirect3DSurface9; break;
    case ObjectKind::VertexBuffer: supported = &kIidIDirect3DVertexBuffer9; break;
    case ObjectKind::IndexBuffer: supported = &kIidIDirect3DIndexBuffer9; break;
    case ObjectKind::VertexDeclaration: supported = &kIidIDirect3DVertexDeclaration9; break;
    case ObjectKind::Texture: supported = &kIidIDirect3DTexture9; break;
    case ObjectKind::CubeTexture: supported = &kIidIDirect3DCubeTexture9; break;
    case ObjectKind::Query: supported = &kIidIDirect3DQuery9; break;
    case ObjectKind::PixelShader: supported = &kIidIDirect3DPixelShader9; break;
    case ObjectKind::VertexShader: supported = &kIidIDirect3DVertexShader9; break;
    case ObjectKind::VolumeTexture: supported = &kIidIDirect3DVolumeTexture9; break;
    case ObjectKind::Volume: supported = &kIidIDirect3DVolume9; break;
    case ObjectKind::StateBlock: supported = &kIidIDirect3DStateBlock9; break;
    case ObjectKind::SwapChain: supported = &kIidIDirect3DSwapChain9; break;
    case ObjectKind::ConstantTable: supported = &kIidID3DXConstantTable36; break;
    }
    return std::memcmp(&iid, &kIidIUnknown, sizeof(iid)) == 0 ||
           std::memcmp(&iid, supported, sizeof(iid)) == 0 ||
           ((kind == ObjectKind::Texture || kind == ObjectKind::CubeTexture ||
             kind == ObjectKind::VolumeTexture) &&
            std::memcmp(&iid, &kIidIDirect3DBaseTexture9, sizeof(iid)) == 0) ||
           ((kind == ObjectKind::Surface || kind == ObjectKind::VertexBuffer ||
             kind == ObjectKind::IndexBuffer || kind == ObjectKind::Texture ||
             kind == ObjectKind::CubeTexture || kind == ObjectKind::VolumeTexture) &&
            std::memcmp(&iid, &kIidIDirect3DResource9, sizeof(iid)) == 0);
}

const char *interface_name(ObjectKind kind)
{
    switch (kind) {
    case ObjectKind::D3D9: return "IDirect3D9";
    case ObjectKind::Device: return "IDirect3DDevice9";
    case ObjectKind::Surface: return "IDirect3DSurface9";
    case ObjectKind::VertexBuffer: return "IDirect3DVertexBuffer9";
    case ObjectKind::IndexBuffer: return "IDirect3DIndexBuffer9";
    case ObjectKind::VertexDeclaration: return "IDirect3DVertexDeclaration9";
    case ObjectKind::Texture: return "IDirect3DTexture9";
    case ObjectKind::CubeTexture: return "IDirect3DCubeTexture9";
    case ObjectKind::Query: return "IDirect3DQuery9";
    case ObjectKind::PixelShader: return "IDirect3DPixelShader9";
    case ObjectKind::VertexShader: return "IDirect3DVertexShader9";
    case ObjectKind::ConstantTable: return "ID3DXConstantTable";
    case ObjectKind::VolumeTexture: return "IDirect3DVolumeTexture9";
    case ObjectKind::Volume: return "IDirect3DVolume9";
    case ObjectKind::StateBlock: return "IDirect3DStateBlock9";
    case ObjectKind::SwapChain: return "IDirect3DSwapChain9";
    }
    return "IUnknown";
}

void report_unsupported_iid(ObjectKind kind, const GUID &iid)
{
    std::ostringstream message;
    message << interface_name(kind) << "::QueryInterface IID {"
            << std::hex << std::setfill('0') << std::setw(8) << iid.Data1 << '-'
            << std::setw(4) << iid.Data2 << '-' << std::setw(4) << iid.Data3 << '-';
    for (uint8_t byte : iid.Data4)
        message << std::setw(2) << static_cast<unsigned>(byte);
    message << "} is not implemented by the bridge; native QueryInterface was not called";
    set_error(message.str());
}

uintptr_t host_window(uint32_t guest_hwnd, bool *ok)
{
    *ok = true;
    if (!guest_hwnd) return 0;
    if (!g_callbacks.guest_hwnd_to_host) {
        set_error("non-null guest HWND has no host-window translation callback");
        *ok = false;
        return 0;
    }
    uintptr_t result = g_callbacks.guest_hwnd_to_host(g_context, guest_hwnd);
    if (!result) {
        set_error("guest HWND could not be translated to a host HWND");
        *ok = false;
    }
    return result;
}

D3DPRESENT_PARAMETERS host_present_parameters(const GuestPresentParameters &guest,
                                                uintptr_t host_hwnd)
{
    D3DPRESENT_PARAMETERS host{};
    host.BackBufferWidth = guest.back_buffer_width;
    host.BackBufferHeight = guest.back_buffer_height;
    host.BackBufferFormat = static_cast<D3DFORMAT>(guest.back_buffer_format);
    host.BackBufferCount = guest.back_buffer_count;
    host.MultiSampleType = static_cast<D3DMULTISAMPLE_TYPE>(guest.multi_sample_type);
    host.MultiSampleQuality = guest.multi_sample_quality;
    host.SwapEffect = static_cast<D3DSWAPEFFECT>(guest.swap_effect);
    host.hDeviceWindow = reinterpret_cast<HWND>(host_hwnd);
    host.Windowed = guest.windowed;
    host.EnableAutoDepthStencil = guest.enable_auto_depth_stencil;
    host.AutoDepthStencilFormat = static_cast<D3DFORMAT>(guest.auto_depth_stencil_format);
    host.Flags = guest.flags;
    host.FullScreen_RefreshRateInHz = guest.fullscreen_refresh_rate;
    host.PresentationInterval = guest.presentation_interval;
    return host;
}

GuestPresentParameters guest_present_parameters(const D3DPRESENT_PARAMETERS &host,
                                                 uint32_t original_guest_hwnd)
{
    GuestPresentParameters guest{};
    guest.back_buffer_width = host.BackBufferWidth;
    guest.back_buffer_height = host.BackBufferHeight;
    guest.back_buffer_format = static_cast<uint32_t>(host.BackBufferFormat);
    guest.back_buffer_count = host.BackBufferCount;
    guest.multi_sample_type = static_cast<uint32_t>(host.MultiSampleType);
    guest.multi_sample_quality = host.MultiSampleQuality;
    guest.swap_effect = static_cast<uint32_t>(host.SwapEffect);
    guest.device_window = original_guest_hwnd;
    guest.windowed = host.Windowed;
    guest.enable_auto_depth_stencil = host.EnableAutoDepthStencil;
    guest.auto_depth_stencil_format = static_cast<uint32_t>(host.AutoDepthStencilFormat);
    guest.flags = host.Flags;
    guest.fullscreen_refresh_rate = host.FullScreen_RefreshRateInHz;
    guest.presentation_interval = host.PresentationInterval;
    return guest;
}

HRESULT do_query_interface(GuestObject &object, const uint32_t *args,
                            uint32_t *result)
{
    GUID iid{};
    if (!guest_read(args[1], &iid, sizeof(iid))) {
        set_error("QueryInterface IID is unmapped");
        return D3DERR_INVALIDCALL;
    }
    /* Dispatch rejects IIDs without a bridge proxy before reaching native QI. */
    void *output = nullptr;
    HRESULT status = object.native->QueryInterface(iid, &output);
    uint32_t guest_output = 0;
    if (SUCCEEDED(status)) {
        guest_output = wrap_native(static_cast<IUnknown *>(output), object.kind, true);
        if (!guest_output) status = E_OUTOFMEMORY;
    }
    if (!guest_write(args[2], &guest_output, sizeof(guest_output))) {
        if (guest_output) {
            auto it = g_objects.find(guest_output);
            if (it != g_objects.end()) {
                it->second.native->Release();
                if (it->second.guest_references > 1) --it->second.guest_references;
                else {
                    GuestObject retired = it->second;
                    g_guest_by_native.erase(retired.native);
                    g_objects.erase(it);
                    free_guest_object(guest_output, retired);
                }
            }
        }
        set_error("QueryInterface output pointer is unmapped");
        return D3DERR_INVALIDCALL;
    }
    *result = guest_output;
    return status;
}

HRESULT create_device(IDirect3D9 *d3d, const uint32_t *args)
{
    /* this, adapter, type, focus HWND, behavior, present params, device ** */
    uint32_t output_zero = 0;
    if (!guest_write(args[6], &output_zero, sizeof(output_zero))) {
        set_error("CreateDevice output pointer is unmapped");
        return D3DERR_INVALIDCALL;
    }
    GuestPresentParameters guest{};
    if (!guest_read(args[5], &guest, sizeof(guest))) {
        set_error("CreateDevice presentation parameters are unmapped");
        return D3DERR_INVALIDCALL;
    }
    bool focus_ok = false;
    uintptr_t focus = host_window(args[3], &focus_ok);
    if (!focus_ok) return D3DERR_INVALIDCALL;
    bool present_ok = false;
    uintptr_t present_window = host_window(guest.device_window, &present_ok);
    if (!present_ok) return D3DERR_INVALIDCALL;

    D3DPRESENT_PARAMETERS host = host_present_parameters(guest, present_window);
    /* Explicit presentation mapping: present an exclusive-fullscreen request in
       the game's own window without a display-mode change. The guest still
       sees the fullscreen parameters it requested. */
    const bool force_windowed = !guest.windowed &&
        std::getenv("AC1_D3D9_FORCE_WINDOWED") != nullptr &&
        std::strcmp(std::getenv("AC1_D3D9_FORCE_WINDOWED"), "0") != 0;
    if (force_windowed) {
        host.Windowed = TRUE;
        host.FullScreen_RefreshRateInHz = 0;
        std::fprintf(stderr, "AC1 D3D9 presentation mapping: fullscreen %ux%u@%u "
                     "presented windowed (AC1_D3D9_FORCE_WINDOWED)\n",
                     guest.back_buffer_width, guest.back_buffer_height,
                     guest.fullscreen_refresh_rate);
    }
    IDirect3DDevice9 *device = nullptr;
#if defined(_WIN32)
    const bool trace_create_device =
        std::getenv("AC1_D3D9_TRACE_CREATEDEVICE") != nullptr;
    if (trace_create_device) {
        const HWND focus_hwnd = reinterpret_cast<HWND>(focus);
        const HWND present_hwnd = host.hDeviceWindow;
        DWORD focus_owner_thread = focus_hwnd
            ? GetWindowThreadProcessId(focus_hwnd, nullptr) : 0;
        DWORD present_owner_thread = present_hwnd
            ? GetWindowThreadProcessId(present_hwnd, nullptr) : 0;
        STARTUPINFOA startup_info{};
        startup_info.cb = static_cast<DWORD>(sizeof(startup_info));
        GetStartupInfoA(&startup_info);
        RECT client{};
        const BOOL has_client = focus_hwnd && GetClientRect(focus_hwnd, &client);
        std::fprintf(stderr,
            "AC1 D3D9 CreateDevice input: provider=%s caller_tid=%lu adapter=%u type=%u "
            "behavior=0x%08x guest_focus=0x%08x focus=%p focus_valid=%d "
            "client_valid=%d visible=%d iconic=%d foreground=%d style=0x%08lx "
            "focus_owner_tid=%lu client=%ld,%ld..%ld,%ld guest_present_hwnd=0x%08x "
            "present=%p present_valid=%d present_owner_tid=%lu "
            "startup_flags=0x%08lx startup_show=%u\n"
            "AC1 D3D9 CreateDevice PP guest: %ux%u format=%u count=%u ms=%u/%u "
            "swap=%u windowed=%d auto_depth=%d depth_fmt=%u flags=0x%08x "
            "refresh=%u interval=0x%08x\n",
            g_d3d9_provider_path.empty() ? "<platform callback>" : g_d3d9_provider_path.c_str(),
            static_cast<unsigned long>(GetCurrentThreadId()), args[1], args[2], args[4],
            args[3], reinterpret_cast<void *>(focus),
            focus_hwnd ? static_cast<int>(IsWindow(focus_hwnd)) : 0,
            has_client ? 1 : 0,
            focus_hwnd ? static_cast<int>(IsWindowVisible(focus_hwnd)) : 0,
            focus_hwnd ? static_cast<int>(IsIconic(focus_hwnd)) : 0,
            focus_hwnd && GetForegroundWindow() == focus_hwnd ? 1 : 0,
            focus_hwnd ? static_cast<unsigned long>(GetWindowLongPtrW(focus_hwnd, GWL_STYLE)) : 0ul,
            static_cast<unsigned long>(focus_owner_thread),
            static_cast<long>(client.left), static_cast<long>(client.top),
            static_cast<long>(client.right), static_cast<long>(client.bottom),
            guest.device_window, reinterpret_cast<void *>(present_hwnd),
            present_hwnd ? static_cast<int>(IsWindow(present_hwnd)) : 0,
            static_cast<unsigned long>(present_owner_thread),
            static_cast<unsigned long>(startup_info.dwFlags),
            static_cast<unsigned>(startup_info.wShowWindow),
            guest.back_buffer_width, guest.back_buffer_height,
            guest.back_buffer_format, guest.back_buffer_count,
            guest.multi_sample_type, guest.multi_sample_quality,
            guest.swap_effect, guest.windowed, guest.enable_auto_depth_stencil,
            guest.auto_depth_stencil_format, guest.flags,
            guest.fullscreen_refresh_rate, guest.presentation_interval);
    }
#endif
    HRESULT status = d3d->CreateDevice(args[1], static_cast<D3DDEVTYPE>(args[2]),
                                       reinterpret_cast<HWND>(focus), args[4],
                                       &host, &device);
#if defined(_WIN32)
    if (trace_create_device) {
        std::fprintf(stderr,
            "AC1 D3D9 CreateDevice native result: hr=0x%08x device=%p "
            "PP=%ux%u format=%u count=%u ms=%u/%lu swap=%u windowed=%d "
            "auto_depth=%d depth_fmt=%u flags=0x%08lx refresh=%u interval=0x%08x\n",
            static_cast<unsigned>(status), static_cast<void *>(device),
            host.BackBufferWidth, host.BackBufferHeight,
            static_cast<unsigned>(host.BackBufferFormat), host.BackBufferCount,
            static_cast<unsigned>(host.MultiSampleType),
            static_cast<unsigned long>(host.MultiSampleQuality),
            static_cast<unsigned>(host.SwapEffect), host.Windowed,
            host.EnableAutoDepthStencil,
            static_cast<unsigned>(host.AutoDepthStencilFormat),
            static_cast<unsigned long>(host.Flags),
            host.FullScreen_RefreshRateInHz, host.PresentationInterval);
    }
#endif
    GuestPresentParameters updated = guest_present_parameters(host, guest.device_window);
    if (force_windowed) {
        updated.windowed = guest.windowed;
        updated.fullscreen_refresh_rate = guest.fullscreen_refresh_rate;
    }
    if (!guest_write(args[5], &updated, sizeof(updated))) {
        if (device) device->Release();
        set_error("CreateDevice could not write back presentation parameters");
        return D3DERR_INVALIDCALL;
    }
    if (FAILED(status)) return status;
    if (!device) {
        set_error("native CreateDevice succeeded without returning a device");
        return E_FAIL;
    }

    uint32_t guest_device = wrap_native(device, ObjectKind::Device, true);
    if (!guest_device) return E_OUTOFMEMORY;
    auto &metadata = g_objects.at(guest_device);
    metadata.guest_focus = args[3];
    metadata.guest_window = guest.device_window ? guest.device_window : args[3];
    metadata.forced_windowed = force_windowed;
    metadata.fullscreen_refresh = guest.fullscreen_refresh_rate;
    if (!guest_write(args[6], &guest_device, sizeof(guest_device))) {
        GuestObject *wrapped = find_object(guest_device, ObjectKind::Device);
        if (wrapped) {
            wrapped->native->Release();
            GuestObject retired = *wrapped;
            g_guest_by_native.erase(retired.native);
            g_objects.erase(guest_device);
            free_guest_object(guest_device, retired);
        }
        set_error("CreateDevice output pointer became unmapped");
        return D3DERR_INVALIDCALL;
    }
    return status;
}

HRESULT get_device_surface(IDirect3DDevice9 *device, const uint32_t *args,
                           bool depth_stencil)
{
    const uint32_t output_address = depth_stencil ? args[1] : args[2];
    auto *output = static_cast<uint32_t *>(guest_memory(
        output_address, sizeof(uint32_t), AC1_D3D9_GUEST_WRITE));
    if (!output) {
        set_error(depth_stencil
            ? "GetDepthStencilSurface output pointer is unmapped"
            : "GetRenderTarget output pointer is unmapped");
        return D3DERR_INVALIDCALL;
    }
    *output = 0;

    IDirect3DSurface9 *surface = nullptr;
    HRESULT status = depth_stencil
        ? device->GetDepthStencilSurface(&surface)
        : device->GetRenderTarget(args[1], &surface);
    if (FAILED(status)) return status;
    if (!surface) {
        set_error("native surface getter succeeded without returning a surface");
        return E_FAIL;
    }

    const uint32_t guest_surface = wrap_native(
        static_cast<IUnknown *>(surface), ObjectKind::Surface, true);
    if (!guest_surface) return E_OUTOFMEMORY;
    *output = guest_surface;
    return status;
}

template <typename Buffer>
HRESULT create_guest_buffer(IDirect3DDevice9 *device, const uint32_t *args,
                            bool index_buffer)
{
    const auto create_native = [&](Buffer **output) {
        if constexpr (std::is_same<Buffer, IDirect3DIndexBuffer9>::value) {
            return device->CreateIndexBuffer(args[1], args[2],
                static_cast<D3DFORMAT>(args[3]), static_cast<D3DPOOL>(args[4]),
                output, nullptr);
        } else {
            return device->CreateVertexBuffer(args[1], args[2], args[3],
                static_cast<D3DPOOL>(args[4]), output, nullptr);
        }
    };
    if (!args[5]) {
        return create_native(nullptr);
    }

    auto *output = static_cast<uint32_t *>(guest_memory(
        args[5], sizeof(uint32_t), AC1_D3D9_GUEST_WRITE));
    if (!output) {
        set_error(index_buffer
            ? "CreateIndexBuffer output pointer is unmapped"
            : "CreateVertexBuffer output pointer is unmapped");
        return D3DERR_INVALIDCALL;
    }
    *output = 0;

    Buffer *buffer = nullptr;
    HRESULT status = create_native(&buffer);
    if (FAILED(status)) return status;
    if (!buffer) {
        set_error("native D3D9 buffer creation succeeded without returning an object");
        return E_FAIL;
    }

    const ObjectKind kind = index_buffer ? ObjectKind::IndexBuffer
                                         : ObjectKind::VertexBuffer;
    const uint32_t guest_buffer = wrap_native(static_cast<IUnknown *>(buffer), kind, true);
    if (!guest_buffer) return E_OUTOFMEMORY;
    *output = guest_buffer;
    return status;
}

HRESULT create_guest_texture(IDirect3DDevice9 *device, const uint32_t *args)
{
    if (!args[7]) {
        return device->CreateTexture(args[1], args[2], args[3], args[4],
            static_cast<D3DFORMAT>(args[5]), static_cast<D3DPOOL>(args[6]),
            nullptr, nullptr);
    }
    auto *output = static_cast<uint32_t *>(guest_memory(
        args[7], sizeof(uint32_t), AC1_D3D9_GUEST_WRITE));
    if (!output) {
        set_error("IDirect3DDevice9::CreateTexture output pointer is unmapped");
        return D3DERR_INVALIDCALL;
    }
    *output = 0;
    IDirect3DTexture9 *texture = nullptr;
    const HRESULT status = device->CreateTexture(args[1], args[2], args[3], args[4],
        static_cast<D3DFORMAT>(args[5]), static_cast<D3DPOOL>(args[6]),
        &texture, nullptr);
    if (FAILED(status)) return status;
    if (!texture) {
        set_error("native CreateTexture succeeded without returning an object");
        return E_FAIL;
    }
    const uint32_t guest_texture = wrap_native(
        static_cast<IUnknown *>(texture), ObjectKind::Texture, true);
    if (!guest_texture) return E_OUTOFMEMORY;
    *output = guest_texture;
    return status;
}

HRESULT create_guest_cube_texture(IDirect3DDevice9 *device,
                                  const uint32_t *args)
{
    if (!args[6]) {
        return device->CreateCubeTexture(args[1], args[2], args[3],
            static_cast<D3DFORMAT>(args[4]), static_cast<D3DPOOL>(args[5]),
            nullptr, nullptr);
    }
    auto *output = static_cast<uint32_t *>(guest_memory(
        args[6], sizeof(uint32_t), AC1_D3D9_GUEST_WRITE));
    if (!output) {
        set_error("IDirect3DDevice9::CreateCubeTexture output pointer is unmapped");
        return D3DERR_INVALIDCALL;
    }
    *output = 0;

    IDirect3DCubeTexture9 *texture = nullptr;
    const HRESULT status = device->CreateCubeTexture(args[1], args[2], args[3],
        static_cast<D3DFORMAT>(args[4]), static_cast<D3DPOOL>(args[5]),
        &texture, nullptr);
    if (FAILED(status)) return status;
    if (!texture) {
        set_error("native CreateCubeTexture succeeded without returning an object");
        return E_FAIL;
    }
    const uint32_t guest_texture = wrap_native(
        static_cast<IUnknown *>(texture), ObjectKind::CubeTexture, true);
    if (!guest_texture) return E_OUTOFMEMORY;
    *output = guest_texture;
    return status;
}

HRESULT create_guest_query(IDirect3DDevice9 *device, const uint32_t *args)
{
    if (!args[2])
        return device->CreateQuery(static_cast<D3DQUERYTYPE>(args[1]), nullptr);

    auto *output = static_cast<uint32_t *>(guest_memory(
        args[2], sizeof(uint32_t), AC1_D3D9_GUEST_WRITE));
    if (!output) {
        set_error("IDirect3DDevice9::CreateQuery output pointer is unmapped");
        return D3DERR_INVALIDCALL;
    }
    *output = 0;
    IDirect3DQuery9 *query = nullptr;
    const HRESULT status = device->CreateQuery(
        static_cast<D3DQUERYTYPE>(args[1]), &query);
    if (FAILED(status)) {
        if (query) query->Release();
        return status;
    }
    if (!query) {
        set_error("native CreateQuery succeeded without returning a query object");
        return E_FAIL;
    }
    const uint32_t guest_query = wrap_native(
        static_cast<IUnknown *>(query), ObjectKind::Query, true);
    if (!guest_query) return E_OUTOFMEMORY;
    *output = guest_query;
    return status;
}

HRESULT create_vertex_declaration(IDirect3DDevice9 *device, const uint32_t *args)
{
    if (!args[1]) {
        set_error("IDirect3DDevice9::CreateVertexDeclaration elements are null");
        return D3DERR_INVALIDCALL;
    }

    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1]{};
    const D3DVERTEXELEMENT9 end_marker = D3DDECL_END();
    bool terminated = false;
    for (uint32_t i = 0; i <= MAXD3DDECLLENGTH; ++i) {
        const uint64_t address = static_cast<uint64_t>(args[1]) +
                                 static_cast<uint64_t>(i) * sizeof(D3DVERTEXELEMENT9);
        if (address > UINT32_MAX ||
            !guest_read(static_cast<uint32_t>(address), &elements[i],
                        sizeof(D3DVERTEXELEMENT9))) {
            set_error("CreateVertexDeclaration element array is unmapped or overflows guest address space");
            return D3DERR_INVALIDCALL;
        }
        if (std::memcmp(&elements[i], &end_marker, sizeof(end_marker)) == 0) {
            terminated = true;
            break;
        }
    }
    if (!terminated) {
        set_error("CreateVertexDeclaration element array has no D3DDECL_END within SDK limit");
        return D3DERR_INVALIDCALL;
    }

    auto *output = args[2] ? static_cast<uint32_t *>(guest_memory(
        args[2], sizeof(uint32_t), AC1_D3D9_GUEST_WRITE)) : nullptr;
    if (args[2] && !output) {
        set_error("CreateVertexDeclaration output pointer is unmapped");
        return D3DERR_INVALIDCALL;
    }
    if (output) *output = 0;

    IDirect3DVertexDeclaration9 *declaration = nullptr;
    const HRESULT status = device->CreateVertexDeclaration(
        elements, output ? &declaration : nullptr);
    if (FAILED(status) || !output) return status;
    if (!declaration) {
        set_error("native CreateVertexDeclaration succeeded without returning an object");
        return E_FAIL;
    }
    const uint32_t guest_declaration = wrap_native(
        static_cast<IUnknown *>(declaration), ObjectKind::VertexDeclaration, true);
    if (!guest_declaration) return E_OUTOFMEMORY;
    *output = guest_declaration;
    return status;
}

bool read_shader_bytecode(uint32_t guest_bytecode, DWORD expected_version,
                         const char *shader_kind, std::vector<DWORD> *staged)
{
    constexpr uint32_t kMaxShaderWords = 1u << 20; /* 4 MiB hard guest-read cap. */
    if (!guest_bytecode || !staged) {
        set_error("D3D shader bytecode pointer is null");
        return false;
    }

    uint32_t token = 0;
    if (!guest_read(guest_bytecode, &token, sizeof(token))) {
        set_error("D3D shader version token is unmapped");
        return false;
    }
    const uint32_t major = (token >> 8) & 0xff;
    const uint32_t minor = token & 0xff;
    const bool pixel = (expected_version >> 16) == 0xffff;
    if ((token >> 16) != (expected_version >> 16) || major < 1 || major > 3 ||
        (major == 1 && (pixel ? minor > 4 : minor != 1)) ||
        (major >= 2 && minor != 0)) {
        set_error(std::string("Create") + shader_kind + " has an invalid D3D9 shader version token");
        return false;
    }
    staged->clear();
    staged->reserve(256);
    staged->push_back(token);

    uint32_t cursor = 1;
    while (cursor < kMaxShaderWords) {
        const uint64_t address = static_cast<uint64_t>(guest_bytecode) +
                                 static_cast<uint64_t>(cursor) * sizeof(DWORD);
        if (address > UINT32_MAX) {
            set_error("D3D shader bytecode address overflows the guest ABI");
            return false;
        }
        if (!guest_read(static_cast<uint32_t>(address), &token, sizeof(token))) {
            set_error("D3D shader token stream has no mapped terminator");
            return false;
        }
        staged->push_back(token);
        ++cursor;

        if (token == D3DSIO_END) return true;

        const DWORD opcode = token & D3DSI_OPCODE_MASK;
        uint32_t payload_words = 0;
        if (opcode == D3DSIO_COMMENT) {
            payload_words = (token & D3DSI_COMMENTSIZE_MASK) >> D3DSI_COMMENTSIZE_SHIFT;
        } else if (major >= 2) {
            payload_words = (token & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT;
        } else {
            // SM1 lengths include destination/source operands but no encoded
            // instruction length. Immediate DEF data may contain an END value.
            switch (opcode) {
            case D3DSIO_NOP: case D3DSIO_PHASE: payload_words = 0; break;
            case D3DSIO_TEXCOORD: case D3DSIO_TEX: payload_words = minor == 4 && pixel ? 2 : 1; break;
            case D3DSIO_TEXKILL: case D3DSIO_TEXDEPTH: payload_words = 1; break;
            case D3DSIO_MOV: case D3DSIO_RCP: case D3DSIO_RSQ: case D3DSIO_EXP:
            case D3DSIO_LOG: case D3DSIO_LIT: case D3DSIO_FRC: case D3DSIO_EXPP:
            case D3DSIO_LOGP: case D3DSIO_TEXBEM: case D3DSIO_TEXBEML:
            case D3DSIO_TEXREG2AR: case D3DSIO_TEXREG2GB: case D3DSIO_TEXM3x2PAD:
            case D3DSIO_TEXM3x2TEX: case D3DSIO_TEXM3x3PAD: case D3DSIO_TEXM3x3TEX:
            case D3DSIO_TEXM3x3VSPEC: case D3DSIO_TEXREG2RGB: case D3DSIO_TEXDP3TEX:
            case D3DSIO_TEXM3x2DEPTH: case D3DSIO_TEXDP3: case D3DSIO_TEXM3x3:
                payload_words = 2; break;
            case D3DSIO_ADD: case D3DSIO_SUB: case D3DSIO_MUL: case D3DSIO_DP3:
            case D3DSIO_DP4: case D3DSIO_MIN: case D3DSIO_MAX: case D3DSIO_SLT:
            case D3DSIO_SGE: case D3DSIO_DST: case D3DSIO_M4x4: case D3DSIO_M4x3:
            case D3DSIO_M3x4: case D3DSIO_M3x3: case D3DSIO_M3x2: case D3DSIO_TEXM3x3SPEC:
                payload_words = 3; break;
            case D3DSIO_MAD: case D3DSIO_LRP: case D3DSIO_CND: case D3DSIO_CMP:
            case D3DSIO_DP2ADD: payload_words = 4; break;
            case D3DSIO_DEF: payload_words = 5; break;
            default:
                set_error("unknown SM1 shader opcode " + std::to_string(opcode));
                return false;
            }
        }
        if (payload_words > kMaxShaderWords - cursor) {
            set_error("D3D shader instruction length exceeds the bounded token stream");
            return false;
        }
        for (uint32_t i = 0; i < payload_words; ++i) {
            const uint64_t payload_address = static_cast<uint64_t>(guest_bytecode) +
                static_cast<uint64_t>(cursor) * sizeof(DWORD);
            if (payload_address > UINT32_MAX ||
                !guest_read(static_cast<uint32_t>(payload_address), &token, sizeof(token))) {
                set_error("D3D shader instruction payload is unmapped");
                return false;
            }
            staged->push_back(token);
            ++cursor;
        }
    }
    set_error("D3D shader token stream exceeds the 4 MiB safety bound without END");
    return false;
}

HRESULT create_guest_shader(IDirect3DDevice9 *device, const uint32_t *args,
                            ObjectKind kind)
{
    const bool pixel_shader = kind == ObjectKind::PixelShader;
    const char *shader_kind = pixel_shader ? "PixelShader" : "VertexShader";
    if (!args[2]) {
        set_error(std::string("IDirect3DDevice9::Create") + shader_kind +
                  " output pointer is null");
        return D3DERR_INVALIDCALL;
    }
    auto *output = static_cast<uint32_t *>(guest_memory(
        args[2], sizeof(uint32_t), AC1_D3D9_GUEST_WRITE));
    if (!output) {
        set_error(std::string("IDirect3DDevice9::Create") + shader_kind +
                  " output pointer is unmapped");
        return D3DERR_INVALIDCALL;
    }
    *output = 0;

    std::vector<DWORD> bytecode;
    const DWORD expected_version = pixel_shader ? D3DPS_VERSION(3, 0)
                                                : D3DVS_VERSION(3, 0);
    if (!read_shader_bytecode(args[1], expected_version, shader_kind, &bytecode))
        return D3DERR_INVALIDCALL;

    IUnknown *native_shader = nullptr;
    HRESULT status = S_OK;
    if (pixel_shader) {
        IDirect3DPixelShader9 *shader = nullptr;
        status = device->CreatePixelShader(bytecode.data(), &shader);
        native_shader = static_cast<IUnknown *>(shader);
    } else {
        IDirect3DVertexShader9 *shader = nullptr;
        status = device->CreateVertexShader(bytecode.data(), &shader);
        native_shader = static_cast<IUnknown *>(shader);
    }
    if (FAILED(status)) return status;
    if (!native_shader) {
        set_error(std::string("native Create") + shader_kind +
                  " succeeded without returning an object");
        return E_FAIL;
    }
    const uint32_t guest_shader = wrap_native(native_shader, kind, true);
    if (!guest_shader) return E_OUTOFMEMORY;
    *output = guest_shader;
    return status;
}

uint32_t copy_constant_table_string(uint32_t table_address, const char *value)
{
    if (!value) return 0;
    constexpr uint32_t kMaximumStringBytes = 4096;
    uint32_t length = 0;
    while (length < kMaximumStringBytes && value[length] != '\0') ++length;
    if (length == kMaximumStringBytes) {
        set_error("D3DX constant-table string exceeds the 4096-byte ABI bound");
        return 0;
    }
    const uint32_t allocation_size = length + 1;
    const uint32_t address = g_callbacks.guest_alloc
        ? g_callbacks.guest_alloc(g_context, allocation_size, 1) : 0;
    if (!address) {
        set_error("guest allocation failed for D3DX constant-table string");
        return 0;
    }
    void *destination = guest_memory(address, allocation_size, AC1_D3D9_GUEST_WRITE);
    if (!destination) {
        g_callbacks.guest_free(g_context, address);
        set_error("D3DX constant-table string allocation is not writable");
        return 0;
    }
    std::memcpy(destination, value, allocation_size);
    g_constant_table_allocations[table_address].push_back(address);
    return address;
}

int copy_constant_table_default(uint32_t table_address, const void *native_value,
                                uint32_t byte_count, uint32_t *guest_value_out)
{
    if (!guest_value_out) return -1;
    *guest_value_out = 0;
    if (!native_value || !byte_count) return 1;
    constexpr uint32_t kMaximumDefaultBytes = 1024u * 1024u;
    if (byte_count > kMaximumDefaultBytes) {
        set_error("D3DX constant default value exceeds the 1 MiB staging bound");
        return -2;
    }
    const uint32_t address = g_callbacks.guest_alloc
        ? g_callbacks.guest_alloc(g_context, byte_count, 4) : 0;
    if (!address) {
        set_error("guest allocation failed for D3DX constant default value");
        return -1;
    }
    void *destination = guest_memory(address, byte_count, AC1_D3D9_GUEST_WRITE);
    if (!destination) {
        g_callbacks.guest_free(g_context, address);
        set_error("D3DX constant default allocation is not writable");
        return -1;
    }
    std::memcpy(destination, native_value, byte_count);
    g_constant_table_allocations[table_address].push_back(address);
    *guest_value_out = address;
    return 1;
}

void release_constant_table_allocations(uint32_t table_address)
{
    auto allocations = g_constant_table_allocations.find(table_address);
    if (allocations != g_constant_table_allocations.end()) {
        for (uint32_t address : allocations->second) {
            auto handle = g_constant_handles.find(address);
            if (handle != g_constant_handles.end()) {
                g_guest_handle_by_native.erase({
                    table_address, reinterpret_cast<uintptr_t>(handle->second.native)});
                g_constant_handles.erase(handle);
            }
            if (g_callbacks.guest_free) g_callbacks.guest_free(g_context, address);
        }
        g_constant_table_allocations.erase(allocations);
    }
}

const char *native_constant_handle(uint32_t table_address, uint32_t guest_handle,
                                   bool allow_null)
{
    if (!guest_handle && allow_null) return nullptr;
    auto found = g_constant_handles.find(guest_handle);
    if (found == g_constant_handles.end() || found->second.table != table_address) {
        set_error("D3DX constant-table handle is unknown or belongs to another table");
        return nullptr;
    }
    return found->second.native;
}

uint32_t guest_constant_handle(uint32_t table_address, const char *native_handle)
{
    if (!native_handle) return 0;
    const auto key = std::make_pair(table_address,
        reinterpret_cast<uintptr_t>(native_handle));
    auto existing = g_guest_handle_by_native.find(key);
    if (existing != g_guest_handle_by_native.end()) return existing->second;
    const uint32_t address = g_callbacks.guest_alloc
        ? g_callbacks.guest_alloc(g_context, sizeof(uint32_t), 4) : 0;
    if (!address) {
        set_error("guest allocation failed for opaque D3DX constant handle");
        return 0;
    }
    const uint32_t marker = 0;
    if (!write_guest_word(address, marker)) {
        g_callbacks.guest_free(g_context, address);
        set_error("opaque D3DX constant handle allocation is not writable");
        return 0;
    }
    g_constant_table_allocations[table_address].push_back(address);
    g_constant_handles.emplace(address, GuestConstantHandle{table_address, native_handle});
    g_guest_handle_by_native.emplace(key, address);
    return address;
}

bool constant_table_guest_string(uint32_t address, std::string *value)
{
    value->clear();
    for (uint32_t i = 0; address && i < 4096 && uint64_t(address)+i <= UINT32_MAX; ++i) {
        char c;
        if (!guest_read(address+i,&c,1)) break;
        if (!c) return true;
        value->push_back(c);
    }
    set_error("ID3DXConstantTable name is null, unmapped or exceeds 4095 bytes");
    return false;
}

const char *constant_table_handle(ID3DXConstantTable *table, uint32_t owner,
                                  uint32_t address, bool optional)
{
    if (!address && optional) return nullptr;
    auto found = g_constant_handles.find(address);
    if (found != g_constant_handles.end()) return native_constant_handle(owner,address,false);
    std::string name;
    if (!constant_table_guest_string(address,&name)) return nullptr;
    return table->GetConstantByName(nullptr,name.c_str());
}

HRESULT invoke_constant_table_method(GuestObject &object, uint32_t method,
                                    const uint32_t *args, uint32_t argc)
{
    auto *table = static_cast<ID3DXConstantTable *>(object.native);
    if (method == 5) {
        if (!require_count(argc, 2, "ID3DXConstantTable::GetDesc"))
            return D3DERR_INVALIDCALL;
        auto *output = static_cast<GuestD3DXConstantTableDesc *>(guest_memory(
            args[1], sizeof(GuestD3DXConstantTableDesc), AC1_D3D9_GUEST_WRITE));
        if (!output) {
            set_error("ID3DXConstantTable::GetDesc output is unmapped");
            return D3DERR_INVALIDCALL;
        }
        D3DXCONSTANTTABLE_DESC native_desc{};
        const HRESULT status = table->GetDesc(&native_desc);
        if (FAILED(status)) return status;
        const uint32_t creator = copy_constant_table_string(args[0], native_desc.Creator);
        if (native_desc.Creator && !creator) return E_OUTOFMEMORY;
        const GuestD3DXConstantTableDesc guest_desc{
            creator, native_desc.Version, native_desc.Constants};
        std::memcpy(output, &guest_desc, sizeof(guest_desc));
        return status;
    }
    if (method == 8) {
        if (!require_count(argc, 3, "ID3DXConstantTable::GetConstant"))
            return 0;
        const char *parent = constant_table_handle(table, args[0], args[1], true);
        if (args[1] && !parent) return 0;
        return guest_constant_handle(args[0], table->GetConstant(parent, args[2]));
    }
    if (method == 6) {
        if (!require_count(argc, 4, "ID3DXConstantTable::GetConstantDesc"))
            return D3DERR_INVALIDCALL;
        if (!args[2] || !args[3]) {
            set_error("ID3DXConstantTable::GetConstantDesc requires descriptor/count outputs");
            return D3DERR_INVALIDCALL;
        }
        uint32_t guest_capacity = 0;
        auto *guest_count_out = static_cast<uint32_t *>(guest_memory(
            args[3], sizeof(uint32_t), AC1_D3D9_GUEST_WRITE));
        if (!guest_count_out ||
            !guest_read(args[3], &guest_capacity, sizeof(guest_capacity)) ||
            guest_capacity == 0 || guest_capacity > 64) {
            set_error("ID3DXConstantTable::GetConstantDesc count output is not writable or capacity is outside 1..64");
            return D3DERR_INVALIDCALL;
        }
        const char *handle = constant_table_handle(table, args[0], args[1], false);
        if (!handle) return D3DERR_INVALIDCALL;
        const uint64_t guest_bytes_64 = static_cast<uint64_t>(guest_capacity) *
                                        sizeof(GuestD3DXConstantDesc);
        if (guest_bytes_64 > UINT32_MAX ||
            !guest_memory(args[2], static_cast<uint32_t>(guest_bytes_64),
                          AC1_D3D9_GUEST_WRITE)) {
            set_error("ID3DXConstantTable::GetConstantDesc descriptor output is unmapped");
            return D3DERR_INVALIDCALL;
        }
        std::vector<D3DXCONSTANT_DESC> native_descs(guest_capacity);
        UINT returned = guest_capacity;
        const HRESULT status = table->GetConstantDesc(
            handle, native_descs.data(), &returned);
        *guest_count_out = returned;
        if (FAILED(status)) return status;
        if (returned > guest_capacity) {
            set_error("native GetConstantDesc returned more records than its staged capacity");
            return E_FAIL;
        }
        std::vector<GuestD3DXConstantDesc> guest_descs(returned);
        for (UINT i = 0; i < returned; ++i) {
            const D3DXCONSTANT_DESC &source = native_descs[i];
            const uint32_t name = copy_constant_table_string(args[0], source.Name);
            if (source.Name && !name) return E_OUTOFMEMORY;
            uint32_t default_value = 0;
            const int default_status = copy_constant_table_default(
                args[0], source.DefaultValue, source.Bytes, &default_value);
            if (default_status != 1)
                return default_status == -2 ? D3DERR_NOTAVAILABLE : E_OUTOFMEMORY;
            guest_descs[i] = GuestD3DXConstantDesc{
                name, static_cast<uint32_t>(source.RegisterSet), source.RegisterIndex,
                source.RegisterCount, static_cast<uint32_t>(source.Class),
                static_cast<uint32_t>(source.Type), source.Rows, source.Columns,
                source.Elements, source.StructMembers, source.Bytes, default_value};
        }
        if (returned && !guest_write(args[2], guest_descs.data(),
                returned * static_cast<uint32_t>(sizeof(GuestD3DXConstantDesc)))) {
            set_error("ID3DXConstantTable::GetConstantDesc descriptor output became unmapped");
            return D3DERR_INVALIDCALL;
        }
        return status;
    }
    if (method == 7 || method == 9 || method == 10) {
        const char *handle = constant_table_handle(table,args[0],args[1],method==9);
        if (args[1] && !handle) return method==7 ? UINT32_MAX : 0;
        if (method==7) return table->GetSamplerIndex(handle);
        if (method==10) return guest_constant_handle(args[0],table->GetConstantElement(handle,args[2]));
        std::string name;
        if (!constant_table_guest_string(args[2],&name)) return 0;
        return guest_constant_handle(args[0],table->GetConstantByName(handle,name.c_str()));
    }
    if (method >= 11 && method <= 25 && method != 23) {
        GuestObject *device_object = find_object(args[1],ObjectKind::Device);
        if (!device_object) {
            set_error("ID3DXConstantTable setter received an unknown guest device");
            return D3DERR_INVALIDCALL;
        }
        auto *device=static_cast<IDirect3DDevice9 *>(device_object->native);
        if (method==11) return table->SetDefaults(device);
        const char *handle=constant_table_handle(table,args[0],args[2],false);
        if (!handle) return D3DERR_INVALIDCALL;
        if (method==13) return table->SetBool(device,handle,static_cast<BOOL>(args[3]));
        if (method==15) return table->SetInt(device,handle,static_cast<INT>(args[3]));
        if (method==17) { float f; std::memcpy(&f,&args[3],4); return table->SetFloat(device,handle,f); }
        uint64_t count=1, stride=4;
        if (method==12) { count=args[4]; stride=1; }
        if (method==14 || method==16 || method==18) count=args[4];
        if (method==19 || method==20) { stride=sizeof(D3DXVECTOR4); if (method==20) count=args[4]; }
        if (method>=21) { stride=sizeof(D3DXMATRIX); if (method==22 || method==25) count=args[4]; }
        if (count*stride > 1024*1024) {
            set_error("ID3DXConstantTable setter input exceeds 1 MiB staging bound");
            return D3DERR_INVALIDCALL;
        }
        // Copy to aligned host storage: guest arrays may be byte-aligned.
        std::vector<uint32_t> storage(std::max<size_t>(1,static_cast<size_t>((count*stride+3)/4)));
        if (count && (!args[3] || !guest_read(args[3],storage.data(),static_cast<uint32_t>(count*stride)))) {
            set_error("ID3DXConstantTable setter input is null or unmapped");
            return D3DERR_INVALIDCALL;
        }
        const void *p=storage.data();
        switch (method) {
        case 12: return table->SetValue(device,handle,p,args[4]);
        case 14: return table->SetBoolArray(device,handle,static_cast<const BOOL *>(p),args[4]);
        case 16: return table->SetIntArray(device,handle,static_cast<const INT *>(p),args[4]);
        case 18: return table->SetFloatArray(device,handle,static_cast<const float *>(p),args[4]);
        case 19: return table->SetVector(device,handle,static_cast<const D3DXVECTOR4 *>(p));
        case 20: return table->SetVectorArray(device,handle,static_cast<const D3DXVECTOR4 *>(p),args[4]);
        case 21: return table->SetMatrix(device,handle,static_cast<const D3DXMATRIX *>(p));
        case 22: return table->SetMatrixArray(device,handle,static_cast<const D3DXMATRIX *>(p),args[4]);
        case 24: return table->SetMatrixTranspose(device,handle,static_cast<const D3DXMATRIX *>(p));
        case 25: return table->SetMatrixTransposeArray(device,handle,static_cast<const D3DXMATRIX *>(p),args[4]);
        }
    }
    set_error("ID3DXConstantTable unsupported slot " + std::to_string(method));
    return D3DERR_NOTAVAILABLE;
}

uint32_t invoke_constant_table(GuestObject &object, uint32_t method,
                              const uint32_t *args, uint32_t argc)
{
    if (method == 0) {
        if (!require_count(argc, 3, "ID3DXConstantTable::QueryInterface"))
            return hresult(E_INVALIDARG);
        uint32_t output = 0;
        return hresult(do_query_interface(object, args, &output));
    }
    if (method == 1) {
        if (!require_count(argc, 1, "ID3DXConstantTable::AddRef")) return 0;
        ++object.guest_references;
        object.native->AddRef();
        return object.guest_references;
    }
    if (method == 2) {
        if (!require_count(argc, 1, "ID3DXConstantTable::Release")) return 0;
        if (!object.guest_references) {
            set_error("ID3DXConstantTable::Release called with zero guest references");
            return 0;
        }
        IUnknown *native = object.native;
        const uint32_t guest_address = args[0];
        native->Release();
        const uint32_t guest_count = --object.guest_references;
        if (!guest_count) {
            release_constant_table_allocations(guest_address);
            GuestObject retired = object;
            g_guest_by_native.erase(native);
            g_objects.erase(guest_address);
            free_guest_object(guest_address, retired);
        }
        return guest_count;
    }
#if !defined(_WIN32)
    ac1_d3dx::diagnostic.clear();
#endif
    HRESULT status=invoke_constant_table_method(object, method, args, argc);
#if !defined(_WIN32)
    if (!ac1_d3dx::diagnostic.empty()) set_error(ac1_d3dx::diagnostic);
#endif
    return hresult(status);
}

int scan_shader_for_d3dx(uint32_t guest_bytecode, std::vector<DWORD> *bytecode)
{
    uint32_t version = 0;
    if (!guest_read(guest_bytecode, &version, sizeof(version))) {
        set_error("D3DXGetShaderConstantTable shader version token is unmapped");
        return -1;
    }
    const DWORD pixel = D3DPS_VERSION(3, 0);
    const DWORD vertex = D3DVS_VERSION(3, 0);
    if (version != pixel && version != vertex) {
        set_error("D3DXGetShaderConstantTable only supports observed shader-model-3 profiles");
        return -2;
    }
    if (!read_shader_bytecode(guest_bytecode, version,
            version == pixel ? "PixelShader" : "VertexShader", bytecode))
        return -1;
    return 1;
}

HRESULT get_vertex_declaration(IDirect3DVertexDeclaration9 *declaration,
                               const uint32_t *args)
{
    if (!args[2]) {
        set_error("IDirect3DVertexDeclaration9::GetDeclaration count output is null");
        return D3DERR_INVALIDCALL;
    }
    auto *guest_count = static_cast<uint32_t *>(guest_memory(
        args[2], sizeof(uint32_t), AC1_D3D9_GUEST_WRITE));
    if (!guest_count) {
        set_error("IDirect3DVertexDeclaration9::GetDeclaration count output is unmapped");
        return D3DERR_INVALIDCALL;
    }

    /* pNumElements is output-only. A first call with a null element array
     * obtains its exact size without trusting guest-provided capacity. */
    UINT required = 0;
    HRESULT status = declaration->GetDeclaration(nullptr, &required);
    if (FAILED(status)) return status;
    if (required == 0 || required > MAXD3DDECLLENGTH + 1u) {
        set_error("native GetDeclaration returned an invalid element count");
        return E_FAIL;
    }
    if (!args[1]) {
        *guest_count = required;
        return status;
    }

    const uint32_t byte_count = required * static_cast<uint32_t>(sizeof(D3DVERTEXELEMENT9));
    auto *guest_elements = static_cast<D3DVERTEXELEMENT9 *>(guest_memory(
        args[1], byte_count, AC1_D3D9_GUEST_WRITE));
    if (!guest_elements) {
        set_error("IDirect3DVertexDeclaration9::GetDeclaration element output is unmapped");
        return D3DERR_INVALIDCALL;
    }

    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1]{};
    UINT returned = required;
    status = declaration->GetDeclaration(elements, &returned);
    if (FAILED(status)) return status;
    if (returned == 0 || returned > required ||
        returned > MAXD3DDECLLENGTH + 1u) {
        set_error("native GetDeclaration changed its element count beyond the checked buffer");
        return E_FAIL;
    }
    std::memcpy(guest_elements, elements,
        returned * static_cast<uint32_t>(sizeof(D3DVERTEXELEMENT9)));
    *guest_count = returned;
    return status;
}

uint32_t invoke_d3d9(GuestObject &object, uint32_t method,
                    const uint32_t *args, uint32_t argc)
{
    IDirect3D9 *d3d = static_cast<IDirect3D9 *>(object.native);
    switch (method) {
    case 0: {
        if (!require_count(argc, 3, "IDirect3D9::QueryInterface")) return hresult(D3DERR_INVALIDCALL);
        uint32_t output = 0;
        HRESULT status = do_query_interface(object, args, &output);
        return hresult(status);
    }
    case 1:
        if (!require_count(argc, 1, "IDirect3D9::AddRef")) return 0;
        ++object.guest_references;
        object.native->AddRef();
        return object.guest_references;
    case 2:
        if (!require_count(argc, 1, "IDirect3D9::Release")) return 0;
        if (!object.guest_references) {
            set_error("IDirect3D9::Release called with zero guest references");
            return 0;
        }
        {
            IUnknown *native = object.native;
            uint32_t guest_address = args[0];
            native->Release();
            uint32_t guest_count = --object.guest_references;
            if (guest_count == 0) {
                GuestObject retired = object;
                g_guest_by_native.erase(native);
                g_objects.erase(guest_address);
                free_guest_object(guest_address, retired);
            }
            return guest_count;
        }
    case 3:
        if (!require_count(argc, 2, "IDirect3D9::RegisterSoftwareDevice")) return hresult(D3DERR_INVALIDCALL);
        set_error("IDirect3D9::RegisterSoftwareDevice is unsupported by this bridge");
        return hresult(D3DERR_NOTAVAILABLE);
    case 4:
        if (!require_count(argc, 1, "IDirect3D9::GetAdapterCount")) return 0;
        return d3d->GetAdapterCount();
    case 5: {
        if (!require_count(argc, 4, "IDirect3D9::GetAdapterIdentifier")) return hresult(D3DERR_INVALIDCALL);
        D3DADAPTER_IDENTIFIER9 native{};
        HRESULT status = d3d->GetAdapterIdentifier(args[1], args[2], &native);
        if (FAILED(status)) return hresult(status);
        GuestAdapterIdentifier guest{};
        std::memcpy(guest.driver, native.Driver, sizeof(guest.driver));
        std::memcpy(guest.description, native.Description, sizeof(guest.description));
        std::memcpy(guest.device_name, native.DeviceName, sizeof(guest.device_name));
        guest.driver_version = native.DriverVersion.QuadPart;
        guest.vendor_id = native.VendorId;
        guest.device_id = native.DeviceId;
        guest.subsystem_id = native.SubSysId;
        guest.revision = native.Revision;
        guest.device_identifier = native.DeviceIdentifier;
        guest.whql_level = native.WHQLLevel;
        if (!guest_write(args[3], &guest, sizeof(guest))) {
            set_error("GetAdapterIdentifier output is unmapped");
            return hresult(D3DERR_INVALIDCALL);
        }
        return hresult(status);
    }
    case 6:
        if (!require_count(argc, 3, "IDirect3D9::GetAdapterModeCount")) return 0;
        return d3d->GetAdapterModeCount(args[1], static_cast<D3DFORMAT>(args[2]));
    case 7: {
        if (!require_count(argc, 5, "IDirect3D9::EnumAdapterModes")) return hresult(D3DERR_INVALIDCALL);
        auto *out = static_cast<D3DDISPLAYMODE *>(guest_memory(args[4], sizeof(D3DDISPLAYMODE), AC1_D3D9_GUEST_WRITE));
        if (!out) { set_error("EnumAdapterModes output is unmapped"); return hresult(D3DERR_INVALIDCALL); }
        return hresult(d3d->EnumAdapterModes(args[1], static_cast<D3DFORMAT>(args[2]), args[3], out));
    }
    case 8: {
        if (!require_count(argc, 3, "IDirect3D9::GetAdapterDisplayMode")) return hresult(D3DERR_INVALIDCALL);
        auto *out = static_cast<D3DDISPLAYMODE *>(guest_memory(args[2], sizeof(D3DDISPLAYMODE), AC1_D3D9_GUEST_WRITE));
        if (!out) { set_error("GetAdapterDisplayMode output is unmapped"); return hresult(D3DERR_INVALIDCALL); }
        return hresult(d3d->GetAdapterDisplayMode(args[1], out));
    }
    case 9:
        if (!require_count(argc, 6, "IDirect3D9::CheckDeviceType")) return hresult(D3DERR_INVALIDCALL);
        return hresult(d3d->CheckDeviceType(args[1], static_cast<D3DDEVTYPE>(args[2]),
                                             static_cast<D3DFORMAT>(args[3]),
                                             static_cast<D3DFORMAT>(args[4]), args[5]));
    case 10:
        if (!require_count(argc, 7, "IDirect3D9::CheckDeviceFormat")) return hresult(D3DERR_INVALIDCALL);
        return hresult(d3d->CheckDeviceFormat(args[1], static_cast<D3DDEVTYPE>(args[2]),
                                               static_cast<D3DFORMAT>(args[3]), args[4],
                                               static_cast<D3DRESOURCETYPE>(args[5]),
                                               static_cast<D3DFORMAT>(args[6])));
    case 11: {
        if (!require_count(argc, 7, "IDirect3D9::CheckDeviceMultiSampleType")) return hresult(D3DERR_INVALIDCALL);
        DWORD *quality = args[6] ? static_cast<DWORD *>(guest_memory(args[6], sizeof(DWORD), AC1_D3D9_GUEST_WRITE)) : nullptr;
        if (args[6] && !quality) { set_error("CheckDeviceMultiSampleType output is unmapped"); return hresult(D3DERR_INVALIDCALL); }
        return hresult(d3d->CheckDeviceMultiSampleType(args[1], static_cast<D3DDEVTYPE>(args[2]),
                                                       static_cast<D3DFORMAT>(args[3]), args[4],
                                                       static_cast<D3DMULTISAMPLE_TYPE>(args[5]), quality));
    }
    case 12:
        if (!require_count(argc, 6, "IDirect3D9::CheckDepthStencilMatch")) return hresult(D3DERR_INVALIDCALL);
        return hresult(d3d->CheckDepthStencilMatch(args[1], static_cast<D3DDEVTYPE>(args[2]),
                                                    static_cast<D3DFORMAT>(args[3]),
                                                    static_cast<D3DFORMAT>(args[4]),
                                                    static_cast<D3DFORMAT>(args[5])));
    case 13:
        if (!require_count(argc, 5, "IDirect3D9::CheckDeviceFormatConversion")) return hresult(D3DERR_INVALIDCALL);
        return hresult(d3d->CheckDeviceFormatConversion(args[1], static_cast<D3DDEVTYPE>(args[2]),
                                                         static_cast<D3DFORMAT>(args[3]),
                                                         static_cast<D3DFORMAT>(args[4])));
    case 14: {
        if (!require_count(argc, 4, "IDirect3D9::GetDeviceCaps")) return hresult(D3DERR_INVALIDCALL);
        auto *out = static_cast<D3DCAPS9 *>(guest_memory(args[3], sizeof(D3DCAPS9), AC1_D3D9_GUEST_WRITE));
        if (!out) { set_error("GetDeviceCaps output is unmapped"); return hresult(D3DERR_INVALIDCALL); }
        return hresult(d3d->GetDeviceCaps(args[1], static_cast<D3DDEVTYPE>(args[2]), out));
    }
    case 15: {
        if (!require_count(argc, 2, "IDirect3D9::GetAdapterMonitor")) return 0;
        HMONITOR monitor = d3d->GetAdapterMonitor(args[1]);
        if (!monitor) return 0;
        uintptr_t value = reinterpret_cast<uintptr_t>(monitor);
        if (g_callbacks.host_monitor_to_guest)
            return g_callbacks.host_monitor_to_guest(g_context, value);
        if (value <= UINT32_MAX) return static_cast<uint32_t>(value);
        set_error("64-bit HMONITOR requires host_monitor_to_guest translation");
        return 0;
    }
    case 16:
        if (!require_count(argc, 7, "IDirect3D9::CreateDevice")) return hresult(D3DERR_INVALIDCALL);
        return hresult(create_device(d3d, args));
    default:
        break;
    }
    set_error("IDirect3D9 vtable method index is invalid");
    return hresult(D3DERR_NOTAVAILABLE);
}

HRESULT present_device(IDirect3DDevice9 *device, const uint32_t *args,
                       IDirect3DSwapChain9 *swap = nullptr, DWORD flags = 0)
{
    static_assert(sizeof(RECT) == 16, "guest RECT must remain 16 bytes");
    static_assert(sizeof(RGNDATAHEADER) == 32,
                  "guest RGNDATAHEADER must remain 32 bytes");
    static_assert(offsetof(RGNDATA, Buffer) == sizeof(RGNDATAHEADER),
                  "RGNDATA rectangle array must follow its header");
    RECT source_rect{};
    RECT destination_rect{};
    const RECT *source = nullptr;
    const RECT *destination = nullptr;
    if (args[1]) {
        if (!guest_read(args[1], &source_rect, sizeof(source_rect))) {
            set_error("IDirect3DDevice9::Present source RECT is unmapped");
            return D3DERR_INVALIDCALL;
        }
        source = &source_rect;
    }
    if (args[2]) {
        if (!guest_read(args[2], &destination_rect, sizeof(destination_rect))) {
            set_error("IDirect3DDevice9::Present destination RECT is unmapped");
            return D3DERR_INVALIDCALL;
        }
        destination = &destination_rect;
    }

    uintptr_t host_hwnd_value = 0;
    if (args[3]) {
        bool translated = false;
        host_hwnd_value = host_window(args[3], &translated);
        if (!translated) return D3DERR_INVALIDCALL;
    }
    const HWND host_hwnd = reinterpret_cast<HWND>(host_hwnd_value);

    std::vector<uint8_t> region_storage;
    const RGNDATA *dirty_region = nullptr;
    if (args[4]) {
        RGNDATAHEADER header{};
        if (!guest_read(args[4], &header, sizeof(header))) {
            set_error("IDirect3DDevice9::Present dirty-region header is unmapped");
            return D3DERR_INVALIDCALL;
        }
        const uint64_t expected_region_bytes =
            static_cast<uint64_t>(header.nCount) * sizeof(RECT);
        const uint64_t total_bytes = sizeof(RGNDATAHEADER) + header.nRgnSize;
        const uint64_t guest_end = static_cast<uint64_t>(args[4]) + total_bytes;
        if (header.dwSize != sizeof(RGNDATAHEADER) ||
            header.nRgnSize != expected_region_bytes ||
            total_bytes > 16u * 1024u * 1024u ||
            guest_end > static_cast<uint64_t>(UINT32_MAX) + 1u) {
            set_error("IDirect3DDevice9::Present dirty-region size/layout is invalid or exceeds the 16 MiB bridge bound");
            return D3DERR_INVALIDCALL;
        }
        try {
            region_storage.resize(static_cast<size_t>(total_bytes));
        } catch (...) {
            set_error("guest allocation failed while staging D3D9 Present dirty-region data");
            return E_OUTOFMEMORY;
        }
        std::memcpy(region_storage.data(), &header, sizeof(header));
        if (header.nRgnSize) {
            const uint64_t data_address =
                static_cast<uint64_t>(args[4]) + sizeof(RGNDATAHEADER);
            if (data_address > UINT32_MAX ||
                !guest_read(static_cast<uint32_t>(data_address),
                            region_storage.data() + sizeof(RGNDATAHEADER),
                            header.nRgnSize)) {
                set_error("IDirect3DDevice9::Present dirty-region rectangle data is unmapped");
                return D3DERR_INVALIDCALL;
            }
        }
        dirty_region = reinterpret_cast<const RGNDATA *>(region_storage.data());
    }

    return swap ? swap->Present(source, destination, host_hwnd, dirty_region, flags)
                : device->Present(source, destination, host_hwnd, dirty_region);
}


// All pointer-bearing ABI boundaries pass through these checked adapters. No
// native interface, HWND, lock pointer, or private-data pointer is copied to PE32.
struct MarshalError {};
[[noreturn]] void marshal_error(const std::string &message)
{
    set_error(message);
    throw MarshalError{};
}

template <typename T>
T *mapped(uint32_t address, uint32_t access, uint64_t count = 1, bool optional = false)
{
    if (!address && optional) return nullptr;
    const uint64_t bytes = count * sizeof(T);
    if (!count) return nullptr;
    if (count > UINT32_MAX / sizeof(T) ||
        static_cast<uint64_t>(address) + bytes > uint64_t{UINT32_MAX} + 1)
        marshal_error("guest data span overflows the 32-bit ABI");
    auto *value = static_cast<T *>(guest_memory(address, static_cast<uint32_t>(bytes), access));
    if (!value) {
        std::ostringstream message;
        message << "unmapped guest " << (access == AC1_D3D9_GUEST_READ ? "input" : "output")
                << " at 0x" << std::hex << address << " (" << std::dec << bytes << " bytes)";
        marshal_error(message.str());
    }
    return value;
}
template <typename T> const T *input(uint32_t address, uint64_t count = 1, bool optional = false)
{ return mapped<T>(address, AC1_D3D9_GUEST_READ, count, optional); }
template <typename T> T *output(uint32_t address, uint64_t count = 1)
{ return mapped<T>(address, AC1_D3D9_GUEST_WRITE, count); }

// Structure pointers are staged in naturally aligned storage. Guest x86 code
// may supply unaligned addresses even when the provider runs on ARM64.
template <typename T, typename Call>
HRESULT copied_input(uint32_t address, Call call, uint32_t count = 1, bool optional = false)
{
    const T *source = input<T>(address,count,optional);
    if (!source) return call(static_cast<T *>(nullptr));
    std::vector<T> values(count);
    std::memcpy(values.data(),source,size_t{count}*sizeof(T));
    return call(values.data());
}
template <typename T, typename Call>
HRESULT copied_output(uint32_t address, Call call, uint32_t count = 1)
{
    void *destination = output<uint8_t>(address,uint64_t{count}*sizeof(T));
    std::vector<T> values(count);
    const HRESULT hr = call(count ? values.data() : nullptr);
    if (SUCCEEDED(hr) && count) std::memcpy(destination,values.data(),size_t{count}*sizeof(T));
    return hr;
}

template <typename T> T *native_object(uint32_t address, ObjectKind kind, bool optional = false)
{
    if (!address && optional) return nullptr;
    GuestObject *object = find_object(address, kind);
    if (!object) marshal_error(std::string("expected registered ") + interface_name(kind) + " guest proxy");
    return static_cast<T *>(object->native);
}
IDirect3DBaseTexture9 *native_texture(uint32_t address, bool optional = false)
{
    if (!address && optional) return nullptr;
    auto it = g_objects.find(address);
    if (it == g_objects.end() || (it->second.kind != ObjectKind::Texture &&
        it->second.kind != ObjectKind::CubeTexture && it->second.kind != ObjectKind::VolumeTexture))
        marshal_error("expected registered base/2D/cube/volume texture guest proxy");
    return static_cast<IDirect3DBaseTexture9 *>(it->second.native);
}

template <typename T, typename Call>
HRESULT interface_output(uint32_t address, ObjectKind kind, Call call)
{
    auto *destination = output<uint32_t>(address);
    *destination = 0;
    T *native = nullptr;
    const HRESULT status = call(&native);
    if (FAILED(status)) { if (native) native->Release(); return status; }
    if (!native) return status; // Null getters (unbound textures/shaders) are valid.
    const uint32_t guest = wrap_native(native, kind, true);
    if (!guest) return E_OUTOFMEMORY;
    *destination = guest;
    return status;
}

ObjectKind texture_kind(IDirect3DBaseTexture9 *texture)
{
    switch (texture->GetType()) {
    case D3DRTYPE_TEXTURE: return ObjectKind::Texture;
    case D3DRTYPE_CUBETEXTURE: return ObjectKind::CubeTexture;
    case D3DRTYPE_VOLUMETEXTURE: return ObjectKind::VolumeTexture;
    default: marshal_error("native GetTexture returned an unknown resource type");
    }
}

bool force_windowed_request(const GuestPresentParameters &guest)
{
    const char *value = std::getenv("AC1_D3D9_FORCE_WINDOWED");
    return !guest.windowed && value && std::strcmp(value, "0") != 0;
}
HRESULT reset_or_swapchain(GuestObject &object, const uint32_t *args, bool reset)
{
    auto *device = static_cast<IDirect3DDevice9 *>(object.native);
    GuestPresentParameters requested{};
    std::memcpy(&requested,input<GuestPresentParameters>(args[1]),sizeof(requested));
    auto *destination = output<GuestPresentParameters>(args[1]);
    if (!reset) output<uint32_t>(args[2]);
    bool ok = false;
    const uintptr_t window = host_window(requested.device_window, &ok);
    if (!ok) return D3DERR_INVALIDCALL;
    auto host = host_present_parameters(requested, window);
    const bool forced = force_windowed_request(requested);
    if (forced) { host.Windowed = TRUE; host.FullScreen_RefreshRateInHz = 0; }
    // Default-pool guest references and outstanding locks remain live. The
    // provider decides Reset success / DEVICELOST / INVALIDCALL; never fake it.
    HRESULT status;
    if (reset) status = device->Reset(&host);
    else status = interface_output<IDirect3DSwapChain9>(args[2], ObjectKind::SwapChain,
        [&](auto **out) { return device->CreateAdditionalSwapChain(&host, out); });
    auto updated = guest_present_parameters(host, requested.device_window);
    if (forced) { updated.windowed = requested.windowed; updated.fullscreen_refresh_rate = requested.fullscreen_refresh_rate; }
    std::memcpy(destination,&updated,sizeof(updated));
    if (SUCCEEDED(status)) {
        GuestObject *metadata = &object;
        if (!reset) metadata = find_object(*output<uint32_t>(args[2]), ObjectKind::SwapChain);
        if (metadata) {
            metadata->guest_focus = object.guest_focus;
            metadata->guest_window = requested.device_window ? requested.device_window : object.guest_focus;
            metadata->forced_windowed = forced;
            metadata->fullscreen_refresh = requested.fullscreen_refresh_rate;
        }
    }
    return status;
}

uint64_t primitive_vertices(uint32_t type, uint32_t count)
{
    if (!count) return 0;
    switch (type) {
    case D3DPT_POINTLIST: return count;
    case D3DPT_LINELIST: return uint64_t{count} * 2;
    case D3DPT_LINESTRIP: return uint64_t{count} + 1;
    case D3DPT_TRIANGLELIST: return uint64_t{count} * 3;
    case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return uint64_t{count} + 2;
    default: marshal_error("user-pointer draw has an invalid primitive type");
    }
}
float guest_float(uint32_t bits) { float value; std::memcpy(&value, &bits, 4); return value; }
uint32_t float_bits(float value) { uint32_t bits; std::memcpy(&bits, &value, 4); return bits; }

uint32_t invoke_remaining_device(GuestObject &object, uint32_t method, const uint32_t *a)
{
    auto *d = static_cast<IDirect3DDevice9 *>(object.native);
    switch (method) {
    case 4: return d->GetAvailableTextureMem();
    case 5: return hresult(d->EvictManagedResources());
    case 6: return hresult(interface_output<IDirect3D9>(a[1], ObjectKind::D3D9, [&](auto **p) { return d->GetDirect3D(p); }));
    case 7: return hresult(copied_output<D3DCAPS9>(a[1],[&](auto *p) { return d->GetDeviceCaps(p); }));
    case 8: return hresult(copied_output<D3DDISPLAYMODE>(a[2],[&](auto *p) { return d->GetDisplayMode(a[1],p); }));
    case 9: {
        auto *out = output<uint32_t>(a[1], 4);
        D3DDEVICE_CREATION_PARAMETERS native{};
        const HRESULT hr = d->GetCreationParameters(&native);
        if (SUCCEEDED(hr)) {
            bool ok;
            const auto hwnd = host_window(object.guest_focus, &ok);
            if (!ok || reinterpret_cast<uintptr_t>(native.hFocusWindow) != hwnd)
                marshal_error("GetCreationParameters cannot map the native focus HWND to a guest handle");
            const uint32_t guest[] = {native.AdapterOrdinal, static_cast<uint32_t>(native.DeviceType), object.guest_focus, native.BehaviorFlags};
            std::memcpy(out, guest, sizeof(guest));
        }
        return hresult(hr);
    }
    case 10: return hresult(d->SetCursorProperties(a[1], a[2], native_object<IDirect3DSurface9>(a[3], ObjectKind::Surface)));
    case 11: d->SetCursorPosition(static_cast<int32_t>(a[1]), static_cast<int32_t>(a[2]), a[3]); return 0;
    case 12: return static_cast<uint32_t>(d->ShowCursor(a[1]));
    case 13: return hresult(reset_or_swapchain(object, a, false));
    case 14: {
        const HRESULT hr = interface_output<IDirect3DSwapChain9>(a[2], ObjectKind::SwapChain, [&](auto **p) { return d->GetSwapChain(a[1], p); });
        if (SUCCEEDED(hr)) {
            auto *swap = find_object(*output<uint32_t>(a[2]), ObjectKind::SwapChain);
            if (swap && a[1] == 0) {
                swap->guest_window = object.guest_window;
                swap->forced_windowed = object.forced_windowed;
                swap->fullscreen_refresh = object.fullscreen_refresh;
            }
        }
        return hresult(hr);
    }
    case 15: return d->GetNumberOfSwapChains();
    case 16: return hresult(reset_or_swapchain(object, a, true));
    case 18: return hresult(interface_output<IDirect3DSurface9>(a[4], ObjectKind::Surface, [&](auto **p) { return d->GetBackBuffer(a[1], a[2], static_cast<D3DBACKBUFFER_TYPE>(a[3]), p); }));
    case 19: return hresult(copied_output<D3DRASTER_STATUS>(a[2],[&](auto *p) { return d->GetRasterStatus(a[1],p); }));
    case 20: return hresult(d->SetDialogBoxMode(a[1]));
    case 21: return hresult(copied_input<D3DGAMMARAMP>(a[3],[&](auto *p) { d->SetGammaRamp(a[1],a[2],p); return S_OK; }));
    case 22: return hresult(copied_output<D3DGAMMARAMP>(a[2],[&](auto *p) { d->GetGammaRamp(a[1],p); return S_OK; }));
    case 24:
        if (a[9]) marshal_error("CreateVolumeTexture shared HANDLE translation is unavailable");
        return hresult(interface_output<IDirect3DVolumeTexture9>(a[8], ObjectKind::VolumeTexture, [&](auto **p) { return d->CreateVolumeTexture(a[1], a[2], a[3], a[4], a[5], static_cast<D3DFORMAT>(a[6]), static_cast<D3DPOOL>(a[7]), p, nullptr); }));
    case 28: case 29:
        if (a[8]) marshal_error("CreateRenderTarget/CreateDepthStencilSurface shared HANDLE translation is unavailable");
        return hresult(interface_output<IDirect3DSurface9>(a[7], ObjectKind::Surface, [&](auto **p) {
            return method == 28 ? d->CreateRenderTarget(a[1], a[2], static_cast<D3DFORMAT>(a[3]), static_cast<D3DMULTISAMPLE_TYPE>(a[4]), a[5], a[6], p, nullptr)
                : d->CreateDepthStencilSurface(a[1], a[2], static_cast<D3DFORMAT>(a[3]), static_cast<D3DMULTISAMPLE_TYPE>(a[4]), a[5], a[6], p, nullptr); }));
    case 30: return hresult(d->UpdateSurface(native_object<IDirect3DSurface9>(a[1], ObjectKind::Surface), input<RECT>(a[2], 1, true), native_object<IDirect3DSurface9>(a[3], ObjectKind::Surface), input<POINT>(a[4], 1, true)));
    case 31: return hresult(d->UpdateTexture(native_texture(a[1]), native_texture(a[2])));
    case 32: return hresult(d->GetRenderTargetData(native_object<IDirect3DSurface9>(a[1], ObjectKind::Surface), native_object<IDirect3DSurface9>(a[2], ObjectKind::Surface)));
    case 33: return hresult(d->GetFrontBufferData(a[1], native_object<IDirect3DSurface9>(a[2], ObjectKind::Surface)));
    case 34: return hresult(d->StretchRect(native_object<IDirect3DSurface9>(a[1], ObjectKind::Surface), input<RECT>(a[2], 1, true), native_object<IDirect3DSurface9>(a[3], ObjectKind::Surface), input<RECT>(a[4], 1, true), static_cast<D3DTEXTUREFILTERTYPE>(a[5])));
    case 36:
        if (a[6]) marshal_error("CreateOffscreenPlainSurface shared HANDLE translation is unavailable");
        return hresult(interface_output<IDirect3DSurface9>(a[5], ObjectKind::Surface, [&](auto **p) { return d->CreateOffscreenPlainSurface(a[1], a[2], static_cast<D3DFORMAT>(a[3]), static_cast<D3DPOOL>(a[4]), p, nullptr); }));
    case 43: return hresult(d->Clear(a[1], input<D3DRECT>(a[2], a[1]), a[3], a[4], guest_float(a[5]), a[6]));
    case 44: return hresult(copied_input<D3DMATRIX>(a[2],[&](auto *p) { return d->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(a[1]),p); }));
    case 45: return hresult(copied_output<D3DMATRIX>(a[2],[&](auto *p) { return d->GetTransform(static_cast<D3DTRANSFORMSTATETYPE>(a[1]),p); }));
    case 46: return hresult(copied_input<D3DMATRIX>(a[2],[&](auto *p) { return d->MultiplyTransform(static_cast<D3DTRANSFORMSTATETYPE>(a[1]),p); }));
    case 48: return hresult(copied_output<D3DVIEWPORT9>(a[1],[&](auto *p) { return d->GetViewport(p); }));
    case 49: return hresult(copied_input<D3DMATERIAL9>(a[1],[&](auto *p) { return d->SetMaterial(p); }));
    case 50: return hresult(copied_output<D3DMATERIAL9>(a[1],[&](auto *p) { return d->GetMaterial(p); }));
    case 51: return hresult(copied_input<D3DLIGHT9>(a[2],[&](auto *p) { return d->SetLight(a[1],p); }));
    case 52: return hresult(copied_output<D3DLIGHT9>(a[2],[&](auto *p) { return d->GetLight(a[1],p); }));
    case 53: return hresult(d->LightEnable(a[1], a[2]));
    case 54: return hresult(d->GetLightEnable(a[1], output<BOOL>(a[2])));
    case 55: return hresult(copied_input<float>(a[2],[&](auto *p) { return d->SetClipPlane(a[1],p); },4));
    case 56: return hresult(copied_output<float>(a[2],[&](auto *p) { return d->GetClipPlane(a[1],p); },4));
    case 58: return hresult(d->GetRenderState(static_cast<D3DRENDERSTATETYPE>(a[1]), output<DWORD>(a[2])));
    case 59: return hresult(interface_output<IDirect3DStateBlock9>(a[2], ObjectKind::StateBlock, [&](auto **p) { return d->CreateStateBlock(static_cast<D3DSTATEBLOCKTYPE>(a[1]), p); }));
    case 60: return hresult(d->BeginStateBlock());
    case 61: return hresult(interface_output<IDirect3DStateBlock9>(a[1], ObjectKind::StateBlock, [&](auto **p) { return d->EndStateBlock(p); }));
    case 62: return hresult(copied_input<D3DCLIPSTATUS9>(a[1],[&](auto *p) { return d->SetClipStatus(p); }));
    case 63: return hresult(copied_output<D3DCLIPSTATUS9>(a[1],[&](auto *p) { return d->GetClipStatus(p); }));
    case 64: {
        auto *out = output<uint32_t>(a[2]); *out = 0;
        IDirect3DBaseTexture9 *texture = nullptr;
        const HRESULT hr = d->GetTexture(a[1], &texture);
        if (FAILED(hr)) { if (texture) texture->Release(); return hresult(hr); }
        if (texture) { *out = wrap_native(texture, texture_kind(texture), true); if (!*out) return hresult(E_OUTOFMEMORY); }
        return hresult(hr);
    }
    case 66: return hresult(d->GetTextureStageState(a[1], static_cast<D3DTEXTURESTAGESTATETYPE>(a[2]), output<DWORD>(a[3])));
    case 68: return hresult(d->GetSamplerState(a[1], static_cast<D3DSAMPLERSTATETYPE>(a[2]), output<DWORD>(a[3])));
    case 70: return hresult(d->ValidateDevice(output<DWORD>(a[1])));
    // Palette entries contain four bytes, not native pointers or GDI handles.
    case 71: return hresult(copied_input<PALETTEENTRY>(a[2],[&](auto *p) { return d->SetPaletteEntries(a[1],p); },256));
    case 72: return hresult(copied_output<PALETTEENTRY>(a[2],[&](auto *p) { return d->GetPaletteEntries(a[1],p); },256));
    case 73: return hresult(d->SetCurrentTexturePalette(a[1]));
    case 74: return hresult(d->GetCurrentTexturePalette(output<UINT>(a[1])));
    case 75: return hresult(copied_input<RECT>(a[1],[&](auto *p) { return d->SetScissorRect(p); }));
    case 76: return hresult(copied_output<RECT>(a[1],[&](auto *p) { return d->GetScissorRect(p); }));
    case 77: return hresult(d->SetSoftwareVertexProcessing(a[1]));
    case 78: return static_cast<uint32_t>(d->GetSoftwareVertexProcessing());
    case 79: return hresult(d->SetNPatchMode(guest_float(a[1])));
    case 80: return float_bits(d->GetNPatchMode());
    case 83: {
        const uint64_t vertices = primitive_vertices(a[1], a[2]);
        if (vertices && !a[4]) marshal_error("DrawPrimitiveUP has zero vertex stride");
        if (a[4] && vertices > UINT32_MAX / a[4])
            marshal_error("DrawPrimitiveUP vertex count times stride exceeds the PE32 ABI");
        const HRESULT hr = d->DrawPrimitiveUP(static_cast<D3DPRIMITIVETYPE>(a[1]), a[2], input<uint8_t>(a[3], vertices * a[4]), a[4]);
        if (SUCCEEDED(hr)) trace_frame_success(false, "DrawPrimitiveUP");
        return hresult(hr);
    }
    case 84: {
        const uint64_t indices = primitive_vertices(a[1], a[4]);
        if (a[6] != D3DFMT_INDEX16 && a[6] != D3DFMT_INDEX32) marshal_error("DrawIndexedPrimitiveUP requires INDEX16 or INDEX32 indices");
        const uint32_t index_bytes = a[6] == D3DFMT_INDEX16 ? 2 : 4;
        const auto *index_data = input<uint8_t>(a[5], indices * index_bytes);
        const uint64_t end_vertex = uint64_t{a[2]} + a[3];
        if (end_vertex > UINT32_MAX || (indices && (!a[8] || !a[3]))) marshal_error("DrawIndexedPrimitiveUP vertex range/stride is invalid");
        for (uint64_t i = 0; i < indices; ++i) {
            uint32_t index = 0; std::memcpy(&index, index_data + i * index_bytes, index_bytes);
            if (index < a[2] || index >= end_vertex) marshal_error("DrawIndexedPrimitiveUP index lies outside MinVertexIndex/NumVertices");
        }
        const auto *data = input<uint8_t>(a[7], end_vertex * a[8]);
        const HRESULT hr = d->DrawIndexedPrimitiveUP(static_cast<D3DPRIMITIVETYPE>(a[1]), a[2], a[3], a[4], index_data, static_cast<D3DFORMAT>(a[6]), data, a[8]);
        if (SUCCEEDED(hr)) trace_frame_success(false, "DrawIndexedPrimitiveUP");
        return hresult(hr);
    }
    case 85: return hresult(d->ProcessVertices(a[1], a[2], a[3], native_object<IDirect3DVertexBuffer9>(a[4], ObjectKind::VertexBuffer), native_object<IDirect3DVertexDeclaration9>(a[5], ObjectKind::VertexDeclaration, true), a[6]));
    case 88: return hresult(interface_output<IDirect3DVertexDeclaration9>(a[1], ObjectKind::VertexDeclaration, [&](auto **p) { return d->GetVertexDeclaration(p); }));
    case 89: return hresult(d->SetFVF(a[1]));
    case 90: return hresult(d->GetFVF(output<DWORD>(a[1])));
    case 93: return hresult(interface_output<IDirect3DVertexShader9>(a[1], ObjectKind::VertexShader, [&](auto **p) { return d->GetVertexShader(p); }));
    case 95: case 97: case 99: case 110: case 112: case 114: {
        const bool boolean = method == 99 || method == 114;
        // Native register validation is retained; mapping a checked span first
        // ensures even an invalid native request cannot access guest host memory.
        void *data = output<uint32_t>(a[2], uint64_t{a[3]} * (boolean ? 1 : 4));
        switch (method) {
        case 95: return hresult(d->GetVertexShaderConstantF(a[1], static_cast<float *>(data), a[3]));
        case 97: return hresult(d->GetVertexShaderConstantI(a[1], static_cast<int *>(data), a[3]));
        case 99: return hresult(d->GetVertexShaderConstantB(a[1], static_cast<BOOL *>(data), a[3]));
        case 110: return hresult(d->GetPixelShaderConstantF(a[1], static_cast<float *>(data), a[3]));
        case 112: return hresult(d->GetPixelShaderConstantI(a[1], static_cast<int *>(data), a[3]));
        default: return hresult(d->GetPixelShaderConstantB(a[1], static_cast<BOOL *>(data), a[3]));
        }
    }
    case 101: {
        auto *offset = output<UINT>(a[3]); auto *stride = output<UINT>(a[4]);
        return hresult(interface_output<IDirect3DVertexBuffer9>(a[2], ObjectKind::VertexBuffer, [&](auto **p) { return d->GetStreamSource(a[1], p, offset, stride); }));
    }
    case 102: return hresult(d->SetStreamSourceFreq(a[1], a[2]));
    case 103: return hresult(d->GetStreamSourceFreq(a[1], output<UINT>(a[2])));
    case 105: return hresult(interface_output<IDirect3DIndexBuffer9>(a[1], ObjectKind::IndexBuffer, [&](auto **p) { return d->GetIndices(p); }));
    case 108: return hresult(interface_output<IDirect3DPixelShader9>(a[1], ObjectKind::PixelShader, [&](auto **p) { return d->GetPixelShader(p); }));
    case 115: return hresult(d->DrawRectPatch(a[1], input<float>(a[2], 4), input<D3DRECTPATCH_INFO>(a[3], 1, true)));
    case 116: return hresult(d->DrawTriPatch(a[1], input<float>(a[2], 3), input<D3DTRIPATCH_INFO>(a[3], 1, true)));
    case 117: return hresult(d->DeletePatch(a[1]));
    default: marshal_error("unrecognized IDirect3DDevice9 slot " + std::to_string(method));
    }
}

uint32_t invoke_device(GuestObject &object, uint32_t method,
                       const uint32_t *args, uint32_t argc)
{
    if (method == 0) {
        if (!require_count(argc, 3, "IDirect3DDevice9::QueryInterface")) return hresult(D3DERR_INVALIDCALL);
        uint32_t output = 0;
        return hresult(do_query_interface(object, args, &output));
    }
    if (method == 1) {
        if (!require_count(argc, 1, "IDirect3DDevice9::AddRef")) return 0;
        ++object.guest_references;
        object.native->AddRef();
        return object.guest_references;
    }
    if (method == 2) {
        if (!require_count(argc, 1, "IDirect3DDevice9::Release")) return 0;
        if (!object.guest_references) {
            set_error("IDirect3DDevice9::Release called with zero guest references");
            return 0;
        }
        IUnknown *native = object.native;
        uint32_t guest_address = args[0];
        native->Release();
        uint32_t guest_count = --object.guest_references;
        if (guest_count == 0) {
            GuestObject retired = object;
            g_guest_by_native.erase(native);
            g_objects.erase(guest_address);
            free_guest_object(guest_address, retired);
        }
        return guest_count;
    }
    if (method == 3) {
        if (!require_count(argc, 1, "IDirect3DDevice9::TestCooperativeLevel"))
            return hresult(D3DERR_INVALIDCALL);
        return hresult(static_cast<IDirect3DDevice9 *>(object.native)->TestCooperativeLevel());
    }
    if (method == 17) {
        if (!require_count(argc, 5, "IDirect3DDevice9::Present"))
            return hresult(D3DERR_INVALIDCALL);
#if defined(_WIN32)
        capture_back_buffer(static_cast<IDirect3DDevice9 *>(object.native));
#endif
        const HRESULT status = present_device(
            static_cast<IDirect3DDevice9 *>(object.native), args);
        if (SUCCEEDED(status)) trace_frame_success(true, "Present");
        return hresult(status);
    }
    if (method == 41) {
        if (!require_count(argc, 1, "IDirect3DDevice9::BeginScene"))
            return hresult(D3DERR_INVALIDCALL);
        return hresult(static_cast<IDirect3DDevice9 *>(object.native)->BeginScene());
    }
    if (method == 42) {
        if (!require_count(argc, 1, "IDirect3DDevice9::EndScene"))
            return hresult(D3DERR_INVALIDCALL);
        return hresult(static_cast<IDirect3DDevice9 *>(object.native)->EndScene());
    }
    if (method == 38) {
        if (!require_count(argc, 3, "IDirect3DDevice9::GetRenderTarget"))
            return hresult(D3DERR_INVALIDCALL);
        return hresult(get_device_surface(static_cast<IDirect3DDevice9 *>(object.native),
                                          args, false));
    }
    if (method == 37) {
        if (!require_count(argc, 3, "IDirect3DDevice9::SetRenderTarget"))
            return hresult(D3DERR_INVALIDCALL);
        IDirect3DSurface9 *surface = nullptr;
        if (args[2]) {
            GuestObject *surface_object = find_object(args[2], ObjectKind::Surface);
            if (!surface_object) {
                set_error("SetRenderTarget received an unregistered surface proxy");
                return hresult(D3DERR_INVALIDCALL);
            }
            surface = static_cast<IDirect3DSurface9 *>(surface_object->native);
        }
        return hresult(static_cast<IDirect3DDevice9 *>(object.native)->
            SetRenderTarget(args[1], surface));
    }
    if (method == 39) {
        if (!require_count(argc, 2, "IDirect3DDevice9::SetDepthStencilSurface"))
            return hresult(D3DERR_INVALIDCALL);
        IDirect3DSurface9 *surface = nullptr;
        if (args[1]) {
            GuestObject *surface_object = find_object(args[1], ObjectKind::Surface);
            if (!surface_object) {
                set_error("SetDepthStencilSurface received an unregistered surface proxy");
                return hresult(D3DERR_INVALIDCALL);
            }
            surface = static_cast<IDirect3DSurface9 *>(surface_object->native);
        }
        return hresult(static_cast<IDirect3DDevice9 *>(object.native)->
            SetDepthStencilSurface(surface));
    }
    if (method == 40) {
        if (!require_count(argc, 2, "IDirect3DDevice9::GetDepthStencilSurface"))
            return hresult(D3DERR_INVALIDCALL);
        return hresult(get_device_surface(static_cast<IDirect3DDevice9 *>(object.native),
                                          args, true));
    }
    if (method == 35) {
        if (!require_count(argc, 4, "IDirect3DDevice9::ColorFill"))
            return hresult(D3DERR_INVALIDCALL);
        GuestObject *surface = find_object(args[1], ObjectKind::Surface);
        if (!surface) {
            set_error("IDirect3DDevice9::ColorFill requires a registered surface proxy");
            return hresult(D3DERR_INVALIDCALL);
        }
        RECT rect{};
        const RECT *rect_pointer = nullptr;
        if (args[2]) {
            static_assert(sizeof(RECT) == 16, "guest RECT must remain 16 bytes");
            if (!guest_read(args[2], &rect, sizeof(rect))) {
                set_error("IDirect3DDevice9::ColorFill RECT is unmapped");
                return hresult(D3DERR_INVALIDCALL);
            }
            rect_pointer = &rect;
        }
        return hresult(static_cast<IDirect3DDevice9 *>(object.native)->ColorFill(
            static_cast<IDirect3DSurface9 *>(surface->native), rect_pointer, args[3]));
    }
    if (method == 47) {
        if (!require_count(argc, 2, "IDirect3DDevice9::SetViewport"))
            return hresult(D3DERR_INVALIDCALL);
        static_assert(sizeof(D3DVIEWPORT9) == 24,
                      "guest D3DVIEWPORT9 must remain 24 bytes");
        D3DVIEWPORT9 viewport{};
        if (!guest_read(args[1], &viewport, sizeof(viewport))) {
            set_error("IDirect3DDevice9::SetViewport viewport is unmapped");
            return hresult(D3DERR_INVALIDCALL);
        }
        return hresult(static_cast<IDirect3DDevice9 *>(object.native)->SetViewport(
            &viewport));
    }
    if (method == 86) {
        if (!require_count(argc, 3, "IDirect3DDevice9::CreateVertexDeclaration"))
            return hresult(D3DERR_INVALIDCALL);
        return hresult(create_vertex_declaration(
            static_cast<IDirect3DDevice9 *>(object.native), args));
    }
    if (method == 106) {
        if (!require_count(argc, 3, "IDirect3DDevice9::CreatePixelShader"))
            return hresult(D3DERR_INVALIDCALL);
        return hresult(create_guest_shader(
            static_cast<IDirect3DDevice9 *>(object.native), args,
            ObjectKind::PixelShader));
    }
    if (method == 91) {
        if (!require_count(argc, 3, "IDirect3DDevice9::CreateVertexShader"))
            return hresult(D3DERR_INVALIDCALL);
        return hresult(create_guest_shader(
            static_cast<IDirect3DDevice9 *>(object.native), args,
            ObjectKind::VertexShader));
    }
    if (method == 26 || method == 27) {
        if (!require_count(argc, 7, method == 26
                ? "IDirect3DDevice9::CreateVertexBuffer"
                : "IDirect3DDevice9::CreateIndexBuffer"))
            return hresult(D3DERR_INVALIDCALL);
        return hresult(method == 26
            ? create_guest_buffer<IDirect3DVertexBuffer9>(
                static_cast<IDirect3DDevice9 *>(object.native), args, false)
            : create_guest_buffer<IDirect3DIndexBuffer9>(
                static_cast<IDirect3DDevice9 *>(object.native), args, true));
    }
    if (method == 23) {
        if (!require_count(argc, 9, "IDirect3DDevice9::CreateTexture"))
            return hresult(D3DERR_INVALIDCALL);
        return hresult(create_guest_texture(
            static_cast<IDirect3DDevice9 *>(object.native), args));
    }
    if (method == 25) {
        if (!require_count(argc, 8, "IDirect3DDevice9::CreateCubeTexture"))
            return hresult(D3DERR_INVALIDCALL);
        return hresult(create_guest_cube_texture(
            static_cast<IDirect3DDevice9 *>(object.native), args));
    }
    if (method == 118) {
        if (!require_count(argc, 3, "IDirect3DDevice9::CreateQuery"))
            return hresult(D3DERR_INVALIDCALL);
        return hresult(create_guest_query(
            static_cast<IDirect3DDevice9 *>(object.native), args));
    }
    if (method == 57) {
        if (!require_count(argc, 3, "IDirect3DDevice9::SetRenderState"))
            return hresult(D3DERR_INVALIDCALL);
        return hresult(static_cast<IDirect3DDevice9 *>(object.native)->SetRenderState(
            static_cast<D3DRENDERSTATETYPE>(args[1]), args[2]));
    }
    auto *device = static_cast<IDirect3DDevice9 *>(object.native);
    if (method == 65) {
        if (!require_count(argc, 3, "IDirect3DDevice9::SetTexture"))
            return hresult(D3DERR_INVALIDCALL);
        IDirect3DBaseTexture9 *texture = nullptr;
        if (args[2]) {
            GuestObject *texture_object = find_object(args[2], ObjectKind::Texture);
            if (!texture_object)
                texture_object = find_object(args[2], ObjectKind::CubeTexture);
            if (!texture_object) texture_object = find_object(args[2], ObjectKind::VolumeTexture);
            if (!texture_object) {
                set_error("SetTexture received an unregistered 2D/cube texture proxy");
                return hresult(D3DERR_INVALIDCALL);
            }
            if (texture_object->kind == ObjectKind::Texture)
                texture = static_cast<IDirect3DTexture9 *>(texture_object->native);
            else
                texture = static_cast<IDirect3DBaseTexture9 *>(texture_object->native);
        }
        return hresult(device->SetTexture(args[1], texture));
    }
    if (method == 81) {
        if (!require_count(argc, 4, "IDirect3DDevice9::DrawPrimitive"))
            return hresult(D3DERR_INVALIDCALL);
        const HRESULT status = device->DrawPrimitive(
            static_cast<D3DPRIMITIVETYPE>(args[1]), args[2], args[3]);
        if (SUCCEEDED(status)) trace_frame_success(false, "DrawPrimitive");
        return hresult(status);
    }
    if (method == 82) {
        if (!require_count(argc, 7, "IDirect3DDevice9::DrawIndexedPrimitive"))
            return hresult(D3DERR_INVALIDCALL);
        const HRESULT status = device->DrawIndexedPrimitive(
            static_cast<D3DPRIMITIVETYPE>(args[1]), static_cast<INT>(args[2]),
            args[3], args[4], args[5], args[6]);
        if (SUCCEEDED(status)) trace_frame_success(false, "DrawIndexedPrimitive");
        return hresult(status);
    }
    if (method == 87) {
        if (!require_count(argc, 2, "IDirect3DDevice9::SetVertexDeclaration"))
            return hresult(D3DERR_INVALIDCALL);
        IDirect3DVertexDeclaration9 *declaration = nullptr;
        if (args[1]) {
            GuestObject *declaration_object = find_object(
                args[1], ObjectKind::VertexDeclaration);
            if (!declaration_object) {
                set_error("SetVertexDeclaration received an unregistered declaration proxy");
                return hresult(D3DERR_INVALIDCALL);
            }
            declaration = static_cast<IDirect3DVertexDeclaration9 *>(
                declaration_object->native);
        }
        return hresult(device->SetVertexDeclaration(declaration));
    }
    if (method == 100) {
        if (!require_count(argc, 5, "IDirect3DDevice9::SetStreamSource"))
            return hresult(D3DERR_INVALIDCALL);
        IDirect3DVertexBuffer9 *buffer = nullptr;
        if (args[2]) {
            GuestObject *buffer_object = find_object(args[2], ObjectKind::VertexBuffer);
            if (!buffer_object) {
                set_error("SetStreamSource received an unregistered vertex-buffer proxy");
                return hresult(D3DERR_INVALIDCALL);
            }
            buffer = static_cast<IDirect3DVertexBuffer9 *>(buffer_object->native);
        }
        return hresult(device->SetStreamSource(args[1], buffer, args[3], args[4]));
    }
    if (method == 104) {
        if (!require_count(argc, 2, "IDirect3DDevice9::SetIndices"))
            return hresult(D3DERR_INVALIDCALL);
        IDirect3DIndexBuffer9 *buffer = nullptr;
        if (args[1]) {
            GuestObject *buffer_object = find_object(args[1], ObjectKind::IndexBuffer);
            if (!buffer_object) {
                set_error("SetIndices received an unregistered index-buffer proxy");
                return hresult(D3DERR_INVALIDCALL);
            }
            buffer = static_cast<IDirect3DIndexBuffer9 *>(buffer_object->native);
        }
        return hresult(device->SetIndices(buffer));
    }
    if (method == 67) {
        if (!require_count(argc, 4, "IDirect3DDevice9::SetTextureStageState"))
            return hresult(D3DERR_INVALIDCALL);
        return hresult(device->SetTextureStageState(args[1],
            static_cast<D3DTEXTURESTAGESTATETYPE>(args[2]), args[3]));
    }
    if (method == 69) {
        if (!require_count(argc, 4, "IDirect3DDevice9::SetSamplerState"))
            return hresult(D3DERR_INVALIDCALL);
        return hresult(device->SetSamplerState(args[1],
            static_cast<D3DSAMPLERSTATETYPE>(args[2]), args[3]));
    }
    if (method == 92) {
        if (!require_count(argc, 2, "IDirect3DDevice9::SetVertexShader"))
            return hresult(D3DERR_INVALIDCALL);
        IDirect3DVertexShader9 *shader = nullptr;
        if (args[1]) {
            GuestObject *shader_object = find_object(args[1], ObjectKind::VertexShader);
            if (!shader_object) {
                set_error("SetVertexShader received an unknown guest vertex-shader proxy");
                return hresult(D3DERR_INVALIDCALL);
            }
            shader = static_cast<IDirect3DVertexShader9 *>(shader_object->native);
        }
        return hresult(device->SetVertexShader(shader));
    }
    if (method == 107) {
        if (!require_count(argc, 2, "IDirect3DDevice9::SetPixelShader"))
            return hresult(D3DERR_INVALIDCALL);
        IDirect3DPixelShader9 *shader = nullptr;
        if (args[1]) {
            GuestObject *shader_object = find_object(args[1], ObjectKind::PixelShader);
            if (!shader_object) {
                set_error("SetPixelShader received an unknown guest pixel-shader proxy");
                return hresult(D3DERR_INVALIDCALL);
            }
            shader = static_cast<IDirect3DPixelShader9 *>(shader_object->native);
        }
        return hresult(device->SetPixelShader(shader));
    }
    if (method == 94 || method == 96 || method == 98 ||
        method == 109 || method == 111 || method == 113) {
        const char *method_name = nullptr;
        ShaderConstantData data_kind = ShaderConstantData::VertexFloat;
        if (method == 94) {
            method_name = "IDirect3DDevice9::SetVertexShaderConstantF";
            data_kind = ShaderConstantData::VertexFloat;
        } else if (method == 96) {
            method_name = "IDirect3DDevice9::SetVertexShaderConstantI";
            data_kind = ShaderConstantData::VertexInt;
        } else if (method == 98) {
            method_name = "IDirect3DDevice9::SetVertexShaderConstantB";
            data_kind = ShaderConstantData::VertexBool;
        } else if (method == 109) {
            method_name = "IDirect3DDevice9::SetPixelShaderConstantF";
            data_kind = ShaderConstantData::PixelFloat;
        } else if (method == 111) {
            method_name = "IDirect3DDevice9::SetPixelShaderConstantI";
            data_kind = ShaderConstantData::PixelInt;
        } else {
            method_name = "IDirect3DDevice9::SetPixelShaderConstantB";
            data_kind = ShaderConstantData::PixelBool;
        }
        if (!require_count(argc, 4, method_name)) return hresult(D3DERR_INVALIDCALL);
        const void *data = nullptr;
        HRESULT status = checked_shader_constant_data(device, data_kind,
            args[1], args[2], args[3], &data);
        if (FAILED(status)) return hresult(status);
        switch (method) {
        case 94:
            return hresult(device->SetVertexShaderConstantF(args[1],
                static_cast<const float *>(data), args[3]));
        case 96:
            return hresult(device->SetVertexShaderConstantI(args[1],
                static_cast<const int *>(data), args[3]));
        case 98:
            return hresult(device->SetVertexShaderConstantB(args[1],
                static_cast<const BOOL *>(data), args[3]));
        case 109:
            return hresult(device->SetPixelShaderConstantF(args[1],
                static_cast<const float *>(data), args[3]));
        case 111:
            return hresult(device->SetPixelShaderConstantI(args[1],
                static_cast<const int *>(data), args[3]));
        case 113:
            return hresult(device->SetPixelShaderConstantB(args[1],
                static_cast<const BOOL *>(data), args[3]));
        }
    }
    return invoke_remaining_device(object, method, args);
}

void clear_buffer_lock(GuestObject &object)
{
    /* A shadowed lock points into the persistent buffer shadow; keep it. */
    if (object.buffer_lock_guest_address && !object.buffer_lock_shadowed &&
        g_callbacks.guest_free)
        g_callbacks.guest_free(g_context, object.buffer_lock_guest_address);
    object.buffer_lock_shadowed = false;
    object.buffer_lock_guest_address = 0;
    object.buffer_lock_offset = 0;
    object.buffer_lock_size = 0;
    object.buffer_lock_flags = 0;
    object.buffer_lock_native_pointer = nullptr;
}

HRESULT unlock_native_buffer(GuestObject &object)
{
    if (object.kind == ObjectKind::VertexBuffer)
        return static_cast<IDirect3DVertexBuffer9 *>(object.native)->Unlock();
    if (object.kind == ObjectKind::IndexBuffer)
        return static_cast<IDirect3DIndexBuffer9 *>(object.native)->Unlock();
    set_error("buffer lock cleanup requested for a non-buffer COM object");
    return D3DERR_INVALIDCALL;
}

void abandon_buffer_lock(GuestObject &object)
{
    if (object.kind != ObjectKind::VertexBuffer &&
        object.kind != ObjectKind::IndexBuffer)
        return;
    if (!object.buffer_lock_guest_address && !object.buffer_lock_native_pointer)
        return;
    if (object.buffer_lock_shadowed) {
        /* No native lock is held between a shadowed Lock and its Unlock; write
         * the locked range back as Unlock would before dropping the lock. */
        void *shadow = object.buffer_lock_size
            ? guest_memory(object.buffer_lock_guest_address, object.buffer_lock_size,
                           AC1_D3D9_GUEST_READ) : nullptr;
        void *native_pointer = nullptr;
        const HRESULT status = !shadow ? D3DERR_INVALIDCALL
            : object.kind == ObjectKind::VertexBuffer
            ? static_cast<IDirect3DVertexBuffer9 *>(object.native)->Lock(
                object.buffer_lock_offset, object.buffer_lock_size, &native_pointer,
                object.buffer_lock_flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))
            : static_cast<IDirect3DIndexBuffer9 *>(object.native)->Lock(
                object.buffer_lock_offset, object.buffer_lock_size, &native_pointer,
                object.buffer_lock_flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE));
        if (SUCCEEDED(status)) {
            if (native_pointer) std::memcpy(native_pointer, shadow, object.buffer_lock_size);
            unlock_native_buffer(object);
        } else {
            set_error("locked D3D9 buffer shadow could not be written back during release cleanup");
        }
        clear_buffer_lock(object);
        return;
    }
    if (object.buffer_lock_native_pointer && object.buffer_lock_guest_address &&
        object.buffer_lock_size &&
        (object.buffer_lock_flags & D3DLOCK_READONLY) == 0) {
        void *staging = guest_memory(object.buffer_lock_guest_address,
                                     object.buffer_lock_size,
                                     AC1_D3D9_GUEST_READ);
        if (staging) {
            std::memcpy(object.buffer_lock_native_pointer, staging,
                        object.buffer_lock_size);
        } else {
            set_error("locked D3D9 buffer guest staging became unreadable before release cleanup");
        }
    }
    const HRESULT status = unlock_native_buffer(object);
    if (FAILED(status))
        set_error("native D3D9 buffer Unlock failed while releasing a locked proxy");
    clear_buffer_lock(object);
}

HRESULT get_native_buffer_size(GuestObject &object, uint32_t *size_out)
{
    if (!size_out) return D3DERR_INVALIDCALL;
    if (object.kind == ObjectKind::VertexBuffer) {
        D3DVERTEXBUFFER_DESC desc{};
        const HRESULT status = static_cast<IDirect3DVertexBuffer9 *>(object.native)->GetDesc(&desc);
        if (FAILED(status)) return status;
        *size_out = desc.Size;
        return D3D_OK;
    }
    if (object.kind == ObjectKind::IndexBuffer) {
        D3DINDEXBUFFER_DESC desc{};
        const HRESULT status = static_cast<IDirect3DIndexBuffer9 *>(object.native)->GetDesc(&desc);
        if (FAILED(status)) return status;
        *size_out = desc.Size;
        return D3D_OK;
    }
    set_error("buffer size requested for a non-buffer COM object");
    return D3DERR_INVALIDCALL;
}

/* AC1_D3D9_SHADOW_BUFFERS=0 restores per-lock staging (A/B and debugging). */
bool shadow_buffer_locks_enabled()
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = std::getenv("AC1_D3D9_SHADOW_BUFFERS");
        enabled = !(value && std::strcmp(value, "0") == 0);
    }
    return enabled != 0;
}

HRESULT lock_guest_buffer(GuestObject &object, const uint32_t *args, uint32_t argc)
{
    const char *name = object.kind == ObjectKind::VertexBuffer
        ? "IDirect3DVertexBuffer9::Lock" : "IDirect3DIndexBuffer9::Lock";
    if (!require_count(argc, 5, name)) return D3DERR_INVALIDCALL;
    if (object.buffer_lock_guest_address || object.buffer_lock_native_pointer) {
        set_error(std::string(name) + " called while this proxy already has an outstanding lock");
        return D3DERR_INVALIDCALL;
    }
    if (!args[3]) {
        set_error(std::string(name) + " output pointer is null");
        return D3DERR_INVALIDCALL;
    }
    void *output = guest_memory(args[3], sizeof(uint32_t), AC1_D3D9_GUEST_WRITE);
    if (!output) {
        set_error(std::string(name) + " output pointer is unmapped");
        return D3DERR_INVALIDCALL;
    }

    uint32_t buffer_size = 0;
    const HRESULT desc_status = get_native_buffer_size(object, &buffer_size);
    if (FAILED(desc_status)) return desc_status;
    if (!buffer_size || args[1] >= buffer_size) {
        set_error(std::string(name) + " byte offset is outside the native buffer extent");
        return D3DERR_INVALIDCALL;
    }
    const uint32_t lock_size = args[2] ? args[2] : buffer_size - args[1];
    if (!lock_size || static_cast<uint64_t>(args[1]) + lock_size > buffer_size) {
        set_error(std::string(name) + " byte range exceeds the native buffer extent");
        return D3DERR_INVALIDCALL;
    }

    const uint32_t flags = args[4];
    const bool discard = (flags & D3DLOCK_DISCARD) != 0;
    if (!supports_guest_buffer_lock_flags(flags)) {
        std::ostringstream message;
        message << name << " flags 0x" << std::hex << flags
                << " are outside the D3D9 lock flag mask";
        set_error(message.str());
        return D3DERR_INVALIDCALL;
    }
    if (!g_callbacks.guest_alloc || !g_callbacks.guest_free) {
        set_error(std::string(name) + " requires guest staging allocation callbacks");
        return D3DERR_INVALIDCALL;
    }
    /* READONLY locks keep private staging: guest writes through them must not
     * reach the buffer, and a shared shadow would retain them. */
    if (shadow_buffer_locks_enabled() && (flags & D3DLOCK_READONLY) == 0) {
        /* Persistent guest shadow: the guest copy is authoritative, so Lock
         * returns shadow+offset without reading native (often GPU-mapped,
         * write-combined) memory or allocating, and Unlock writes only the
         * locked range back with the guest DISCARD/NOOVERWRITE flags. The
         * profiler measured per-lock allocation plus native read-back as 53%
         * of the render thread (dynamic vertex buffer at 0x00A26370). */
        if (!object.buffer_shadow) {
            const uint32_t shadow = g_callbacks.guest_alloc(g_context, buffer_size, 16u);
            void *host = shadow &&
                    static_cast<uint64_t>(shadow) + buffer_size <= UINT64_C(0x100000000)
                ? guest_memory(shadow, buffer_size, AC1_D3D9_GUEST_WRITE) : nullptr;
            if (!host) {
                if (shadow) g_callbacks.guest_free(g_context, shadow);
                set_error(std::string(name) + " could not allocate a persistent guest buffer shadow");
                return E_OUTOFMEMORY;
            }
            /* One-time initialization from the native contents; write-only
             * buffers may refuse a read lock, and their contents are undefined. */
            void *native_contents = nullptr;
            const HRESULT read_status = object.kind == ObjectKind::VertexBuffer
                ? static_cast<IDirect3DVertexBuffer9 *>(object.native)->Lock(
                    0, 0, &native_contents, D3DLOCK_READONLY)
                : static_cast<IDirect3DIndexBuffer9 *>(object.native)->Lock(
                    0, 0, &native_contents, D3DLOCK_READONLY);
            if (SUCCEEDED(read_status)) {
                if (native_contents) std::memcpy(host, native_contents, buffer_size);
                else std::memset(host, 0, buffer_size);
                unlock_native_buffer(object);
            } else {
                std::memset(host, 0, buffer_size);
            }
            object.buffer_shadow = shadow;
            object.buffer_shadow_size = buffer_size;
        }
        if (buffer_size > object.buffer_shadow_size) {
            set_error(std::string(name) + " native buffer grew beyond its guest shadow");
            return D3DERR_INVALIDCALL;
        }
        const uint32_t shadow_address = object.buffer_shadow + args[1];
        std::memcpy(output, &shadow_address, sizeof(shadow_address));
        object.buffer_lock_guest_address = shadow_address;
        object.buffer_lock_offset = args[1];
        object.buffer_lock_size = lock_size;
        object.buffer_lock_flags = flags;
        object.buffer_lock_native_pointer = nullptr;
        object.buffer_lock_shadowed = true;
        return D3D_OK;
    }
    const uint32_t staging_address = g_callbacks.guest_alloc(g_context, lock_size, 4u);
    if (!staging_address ||
        static_cast<uint64_t>(staging_address) + lock_size > UINT64_C(0x100000000)) {
        if (staging_address) g_callbacks.guest_free(g_context, staging_address);
        set_error(std::string(name) + " could not allocate a bounded guest lock buffer");
        return E_OUTOFMEMORY;
    }
    void *staging = guest_memory(staging_address, lock_size, AC1_D3D9_GUEST_WRITE);
    if (!staging) {
        g_callbacks.guest_free(g_context, staging_address);
        set_error(std::string(name) + " guest staging span is not writable");
        return D3DERR_INVALIDCALL;
    }

    void *native_pointer = nullptr;
    const HRESULT status = object.kind == ObjectKind::VertexBuffer
        ? static_cast<IDirect3DVertexBuffer9 *>(object.native)->Lock(
            args[1], args[2], &native_pointer, flags)
        : static_cast<IDirect3DIndexBuffer9 *>(object.native)->Lock(
            args[1], args[2], &native_pointer, flags);
    if (FAILED(status)) {
        g_callbacks.guest_free(g_context, staging_address);
        return status;
    }
    if (!native_pointer) {
        const HRESULT unlock_status = unlock_native_buffer(object);
        if (FAILED(unlock_status)) {
            object.buffer_lock_guest_address = staging_address;
            object.buffer_lock_offset = args[1];
            object.buffer_lock_size = lock_size;
            object.buffer_lock_flags = flags;
        } else {
            g_callbacks.guest_free(g_context, staging_address);
        }
        set_error(std::string(name) + " native Lock succeeded with a null host mapping");
        return E_FAIL;
    }
    if (!discard)
        std::memcpy(staging, native_pointer, lock_size);

    std::memcpy(output, &staging_address, sizeof(staging_address));
    object.buffer_lock_guest_address = staging_address;
    object.buffer_lock_offset = args[1];
    object.buffer_lock_size = lock_size;
    object.buffer_lock_flags = flags;
    object.buffer_lock_native_pointer = native_pointer;
    return status;
}

HRESULT unlock_guest_buffer(GuestObject &object, uint32_t argc)
{
    const char *name = object.kind == ObjectKind::VertexBuffer
        ? "IDirect3DVertexBuffer9::Unlock" : "IDirect3DIndexBuffer9::Unlock";
    if (!require_count(argc, 1, name)) return D3DERR_INVALIDCALL;
    if (object.buffer_lock_shadowed) {
        if (!object.buffer_lock_size) {
            set_error(std::string(name) + " called without a matching successful Lock");
            return D3DERR_INVALIDCALL;
        }
        HRESULT status = D3D_OK;
        if ((object.buffer_lock_flags & D3DLOCK_READONLY) == 0) {
            void *shadow = guest_memory(object.buffer_lock_guest_address,
                                        object.buffer_lock_size, AC1_D3D9_GUEST_READ);
            const DWORD write_flags = object.buffer_lock_flags &
                (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE | D3DLOCK_NOSYSLOCK);
            void *native_pointer = nullptr;
            status = !shadow ? D3DERR_INVALIDCALL
                : object.kind == ObjectKind::VertexBuffer
                ? static_cast<IDirect3DVertexBuffer9 *>(object.native)->Lock(
                    object.buffer_lock_offset, object.buffer_lock_size, &native_pointer, write_flags)
                : static_cast<IDirect3DIndexBuffer9 *>(object.native)->Lock(
                    object.buffer_lock_offset, object.buffer_lock_size, &native_pointer, write_flags);
            if (SUCCEEDED(status)) {
                if (native_pointer)
                    std::memcpy(native_pointer, shadow, object.buffer_lock_size);
                const HRESULT unlock_status = unlock_native_buffer(object);
                if (!native_pointer) status = E_FAIL;
                else if (FAILED(unlock_status)) status = unlock_status;
            }
            if (FAILED(status))
                set_error(std::string(name) + " could not write the guest shadow back to the native buffer");
        }
        clear_buffer_lock(object);
        return status;
    }
    if (!object.buffer_lock_guest_address || !object.buffer_lock_native_pointer ||
        !object.buffer_lock_size) {
        set_error(std::string(name) + " called without a matching successful Lock");
        return D3DERR_INVALIDCALL;
    }

    const bool read_only = (object.buffer_lock_flags & D3DLOCK_READONLY) != 0;
    if (!read_only) {
        void *staging = guest_memory(object.buffer_lock_guest_address,
                                     object.buffer_lock_size,
                                     AC1_D3D9_GUEST_READ);
        if (!staging) {
            const HRESULT unlock_status = unlock_native_buffer(object);
            if (SUCCEEDED(unlock_status)) clear_buffer_lock(object);
            set_error(std::string(name) + " guest lock buffer became unreadable before writeback");
            return D3DERR_INVALIDCALL;
        }
        std::memcpy(object.buffer_lock_native_pointer, staging,
                    object.buffer_lock_size);
    }

    const HRESULT status = unlock_native_buffer(object);
    if (SUCCEEDED(status)) clear_buffer_lock(object);
    return status;
}

uint32_t invoke_buffer(GuestObject &object, uint32_t method,
                       const uint32_t *args, uint32_t argc)
{
    if (method == 0) {
        if (!require_count(argc, 3, "IDirect3D buffer::QueryInterface"))
            return hresult(D3DERR_INVALIDCALL);
        uint32_t output = 0;
        return hresult(do_query_interface(object, args, &output));
    }
    if (method == 1) {
        if (!require_count(argc, 1, "IDirect3D buffer::AddRef")) return 0;
        ++object.guest_references;
        object.native->AddRef();
        return object.guest_references;
    }
    if (method == 2) {
        if (!require_count(argc, 1, "IDirect3D buffer::Release")) return 0;
        if (!object.guest_references) {
            set_error("IDirect3D buffer::Release called with zero guest references");
            return 0;
        }
        IUnknown *native = object.native;
        const uint32_t guest_address = args[0];
        if (object.guest_references == 1u) abandon_buffer_lock(object);
        native->Release();
        const uint32_t guest_count = --object.guest_references;
        if (!guest_count) {
            GuestObject retired = object;
            g_guest_by_native.erase(native);
            g_objects.erase(guest_address);
            free_guest_object(guest_address, retired);
        }
        return guest_count;
    }
    if (method == 11) return hresult(lock_guest_buffer(object, args, argc));
    if (method == 12) return hresult(unlock_guest_buffer(object, argc));
    set_error(object.kind == ObjectKind::VertexBuffer
        ? "IDirect3DVertexBuffer9 method is not implemented"
        : "IDirect3DIndexBuffer9 method is not implemented");
    return hresult(D3DERR_NOTAVAILABLE);
}

uint32_t invoke_query(GuestObject &object, uint32_t method,
                      const uint32_t *args, uint32_t argc)
{
    auto *query = static_cast<IDirect3DQuery9 *>(object.native);
    if (method == 0) {
        if (!require_count(argc, 3, "IDirect3DQuery9::QueryInterface"))
            return hresult(D3DERR_INVALIDCALL);
        uint32_t output = 0;
        return hresult(do_query_interface(object, args, &output));
    }
    if (method == 1) {
        if (!require_count(argc, 1, "IDirect3DQuery9::AddRef")) return 0;
        ++object.guest_references;
        object.native->AddRef();
        return object.guest_references;
    }
    if (method == 2) {
        if (!require_count(argc, 1, "IDirect3DQuery9::Release")) return 0;
        if (!object.guest_references) {
            set_error("IDirect3DQuery9::Release called with zero guest references");
            return 0;
        }
        IUnknown *native = object.native;
        const uint32_t guest_address = args[0];
        native->Release();
        const uint32_t guest_count = --object.guest_references;
        if (!guest_count) {
            GuestObject retired = object;
            g_guest_by_native.erase(native);
            g_objects.erase(guest_address);
            free_guest_object(guest_address, retired);
        }
        return guest_count;
    }
    if (method == 4) {
        if (!require_count(argc, 1, "IDirect3DQuery9::GetType"))
            return hresult(D3DERR_INVALIDCALL);
        return static_cast<uint32_t>(query->GetType());
    }
    if (method == 5) {
        if (!require_count(argc, 1, "IDirect3DQuery9::GetDataSize"))
            return hresult(D3DERR_INVALIDCALL);
        return query->GetDataSize();
    }
    if (method == 6) {
        if (!require_count(argc, 2, "IDirect3DQuery9::Issue"))
            return hresult(D3DERR_INVALIDCALL);
        if (args[1] != D3DISSUE_BEGIN && args[1] != D3DISSUE_END) {
            set_error("IDirect3DQuery9::Issue requires BEGIN or END flags");
            return hresult(D3DERR_INVALIDCALL);
        }
        return hresult(query->Issue(args[1]));
    }
    if (method == 7) {
        const uint32_t expected = query->GetDataSize();
        if (args[1] && args[2] != expected) {
            set_error("IDirect3DQuery9::GetData size differs from native GetDataSize");
            return hresult(D3DERR_INVALIDCALL);
        }
        void *data = args[1] ? output<uint8_t>(args[1], args[2]) : nullptr;
        return hresult(query->GetData(data,args[2],args[3]));
    }
    set_error("IDirect3DQuery9 method is not implemented");
    return hresult(D3DERR_NOTAVAILABLE);
}

uint32_t invoke_vertex_declaration(GuestObject &object, uint32_t method,
                                   const uint32_t *args, uint32_t argc)
{
    if (method == 0) {
        if (!require_count(argc, 3, "IDirect3DVertexDeclaration9::QueryInterface"))
            return hresult(D3DERR_INVALIDCALL);
        uint32_t output = 0;
        return hresult(do_query_interface(object, args, &output));
    }
    if (method == 1) {
        if (!require_count(argc, 1, "IDirect3DVertexDeclaration9::AddRef")) return 0;
        ++object.guest_references;
        object.native->AddRef();
        return object.guest_references;
    }
    if (method == 2) {
        if (!require_count(argc, 1, "IDirect3DVertexDeclaration9::Release")) return 0;
        if (!object.guest_references) {
            set_error("IDirect3DVertexDeclaration9::Release called with zero guest references");
            return 0;
        }
        IUnknown *native = object.native;
        const uint32_t guest_address = args[0];
        native->Release();
        const uint32_t guest_count = --object.guest_references;
        if (!guest_count) {
            GuestObject retired = object;
            g_guest_by_native.erase(native);
            g_objects.erase(guest_address);
            free_guest_object(guest_address, retired);
        }
        return guest_count;
    }
    if (method == 4) {
        if (!require_count(argc, 3, "IDirect3DVertexDeclaration9::GetDeclaration"))
            return hresult(D3DERR_INVALIDCALL);
        return hresult(get_vertex_declaration(
            static_cast<IDirect3DVertexDeclaration9 *>(object.native), args));
    }
    set_error("IDirect3DVertexDeclaration9 method is not implemented");
    return hresult(D3DERR_NOTAVAILABLE);
}

struct UnsupportedMarshal : MarshalError {};
[[noreturn]] void unsupported(const std::string &message)
{
    set_error(message);
    throw UnsupportedMarshal{};
}
struct StagedLock {
    void *native_bits = nullptr;
    uint32_t guest_bits = 0, size = 0, pitch = 0, slice = 0;
    uint32_t row_bytes = 0, rows = 0, slices = 0, flags = 0;
};
std::map<TextureLockKey, StagedLock> g_staged_locks;
// The native private-data store owns its IUnknown references. This table only
// remembers the interface ABI needed to translate their pointer-sized payloads.
std::map<IUnknown *, std::map<std::string, ObjectKind>> g_private_interfaces;

struct FormatLayout { uint32_t width, height, bytes; };
FormatLayout format_layout(D3DFORMAT format)
{
    switch (format) {
    case D3DFMT_DXT1: return {4,4,8};
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: return {4,4,16};
    case D3DFMT_UYVY: case D3DFMT_YUY2: case D3DFMT_R8G8_B8G8: case D3DFMT_G8R8_G8B8: return {2,1,4};
    case D3DFMT_R3G3B2: case D3DFMT_A8: case D3DFMT_P8: case D3DFMT_L8: case D3DFMT_A4L4: return {1,1,1};
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4: case D3DFMT_A8R3G3B2: case D3DFMT_X4R4G4B4:
    case D3DFMT_A8P8: case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L6V5U5:
    case D3DFMT_L16: case D3DFMT_R16F: case D3DFMT_CxV8U8:
    case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: case D3DFMT_D15S1: return {1,1,2};
    case D3DFMT_R8G8B8: return {1,1,3};
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8: case D3DFMT_A2B10G10R10: case D3DFMT_A2R10G10B10:
    case D3DFMT_G16R16: case D3DFMT_Q8W8V8U8: case D3DFMT_V16U16:
    case D3DFMT_X8L8V8U8: case D3DFMT_A2W10V10U10: case D3DFMT_G16R16F:
    case D3DFMT_R32F: case D3DFMT_D32: case D3DFMT_D24S8: case D3DFMT_D24X8:
    case D3DFMT_D24X4S4: case D3DFMT_D32F_LOCKABLE: case D3DFMT_D24FS8: return {1,1,4};
    case D3DFMT_A16B16G16R16: case D3DFMT_Q16W16V16U16:
    case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: return {1,1,8};
    case D3DFMT_A32B32G32R32F: return {1,1,16};
    default: unsupported("lock staging cannot derive the byte layout of D3DFORMAT " + std::to_string(static_cast<uint32_t>(format)));
    }
}

HRESULT unlock_staged_native(GuestObject &object, const TextureLockKey &key)
{
    switch (object.kind) {
    case ObjectKind::Surface: return static_cast<IDirect3DSurface9 *>(object.native)->UnlockRect();
    case ObjectKind::Texture: return static_cast<IDirect3DTexture9 *>(object.native)->UnlockRect(key.level);
    case ObjectKind::CubeTexture: return static_cast<IDirect3DCubeTexture9 *>(object.native)->UnlockRect(static_cast<D3DCUBEMAP_FACES>(key.face), key.level);
    case ObjectKind::VolumeTexture: return static_cast<IDirect3DVolumeTexture9 *>(object.native)->UnlockBox(key.level);
    case ObjectKind::Volume: return static_cast<IDirect3DVolume9 *>(object.native)->UnlockBox();
    default: return D3DERR_INVALIDCALL;
    }
}
bool copy_staged(const StagedLock &lock, bool to_native)
{
    if (to_native && (lock.flags & D3DLOCK_READONLY)) return true;
    auto *guest = static_cast<uint8_t *>(guest_memory(lock.guest_bits, lock.size,
        to_native ? AC1_D3D9_GUEST_READ : AC1_D3D9_GUEST_WRITE));
    if (!guest) { set_error("lock staging buffer is unmapped during copy"); return false; }
    for (uint32_t z = 0; z < lock.slices; ++z)
        for (uint32_t y = 0; y < lock.rows; ++y) {
            const size_t offset = size_t{z} * lock.slice + size_t{y} * lock.pitch;
            auto *native = static_cast<uint8_t *>(lock.native_bits) + offset;
            if (to_native) std::memcpy(native, guest + offset, lock.row_bytes);
            else std::memcpy(guest + offset, native, lock.row_bytes);
        }
    return true;
}
HRESULT unlock_staged(GuestObject &object, const TextureLockKey &key)
{
    const auto it = g_staged_locks.find(key);
    if (it == g_staged_locks.end()) { set_error("UnlockRect/UnlockBox has no matching guest lock"); return D3DERR_INVALIDCALL; }
    // Keep a lock available for retry when guest memory could not be read.
    if (!copy_staged(it->second, true)) return D3DERR_INVALIDCALL;
    const HRESULT hr = unlock_staged_native(object, key);
    g_callbacks.guest_free(g_context, it->second.guest_bits);
    g_staged_locks.erase(it);
    return hr;
}
void discard_staged_locks(uint32_t address, GuestObject &object)
{
    auto it = g_staged_locks.lower_bound({address,0,0});
    while (it != g_staged_locks.end() && it->first.guest_texture == address) {
        copy_staged(it->second, true);
        unlock_staged_native(object, it->first);
        g_callbacks.guest_free(g_context, it->second.guest_bits);
        it = g_staged_locks.erase(it);
    }
}

HRESULT lock_staged(GuestObject &object, uint32_t address, uint32_t face,
                    uint32_t level, uint32_t out_address, uint32_t region_address,
                    uint32_t flags, bool volume)
{
    const TextureLockKey key{address, face, level};
    if (g_staged_locks.count(key)) { set_error("duplicate LockRect/LockBox for the same subresource"); return D3DERR_INVALIDCALL; }
    void *out = output<uint8_t>(out_address, volume ? sizeof(GuestLockedBox) : sizeof(GuestLockedRect));
    // Validate guest pointers before invoking any provider method.
    if (region_address) {
        if (volume) input<D3DBOX>(region_address);
        else input<RECT>(region_address);
    }
    if (object.kind == ObjectKind::CubeTexture && face > D3DCUBEMAP_FACE_NEGATIVE_Z)
        return D3DERR_INVALIDCALL;
    D3DFORMAT format;
    uint32_t width, height, depth = 1;
    HRESULT hr;
    if (volume) {
        D3DVOLUME_DESC desc{};
        hr = object.kind == ObjectKind::Volume
            ? static_cast<IDirect3DVolume9 *>(object.native)->GetDesc(&desc)
            : static_cast<IDirect3DVolumeTexture9 *>(object.native)->GetLevelDesc(level, &desc);
        format = desc.Format; width = desc.Width; height = desc.Height; depth = desc.Depth;
    } else {
        D3DSURFACE_DESC desc{};
        if (object.kind == ObjectKind::Surface) hr = static_cast<IDirect3DSurface9 *>(object.native)->GetDesc(&desc);
        else if (object.kind == ObjectKind::Texture) hr = static_cast<IDirect3DTexture9 *>(object.native)->GetLevelDesc(level, &desc);
        else hr = static_cast<IDirect3DCubeTexture9 *>(object.native)->GetLevelDesc(level, &desc);
        format = desc.Format; width = desc.Width; height = desc.Height;
    }
    if (FAILED(hr)) return hr;
    const auto layout = format_layout(format);
    RECT rect{}; D3DBOX box{};
    const RECT *rect_ptr = nullptr; const D3DBOX *box_ptr = nullptr;
    uint32_t left = 0, top = 0;
    if (region_address) {
        if (volume) {
            std::memcpy(&box,input<D3DBOX>(region_address),sizeof(box)); box_ptr = &box;
            if (box.Left >= box.Right || box.Top >= box.Bottom || box.Front >= box.Back ||
                box.Right > width || box.Bottom > height || box.Back > depth)
                marshal_error("LockBox region is outside its volume dimensions");
            left = box.Left; top = box.Top;
            width = box.Right - box.Left; height = box.Bottom - box.Top; depth = box.Back - box.Front;
        } else {
            std::memcpy(&rect,input<RECT>(region_address),sizeof(rect)); rect_ptr = &rect;
            if (rect.left < 0 || rect.top < 0 || rect.right <= rect.left || rect.bottom <= rect.top ||
                static_cast<uint32_t>(rect.right) > width || static_cast<uint32_t>(rect.bottom) > height)
                marshal_error("LockRect region is outside its surface dimensions");
            left = rect.left; top = rect.top;
            width = rect.right - rect.left; height = rect.bottom - rect.top;
        }
    }
    if (left % layout.width || top % layout.height)
        marshal_error("lock region origin is not aligned to its format block");
    const uint64_t row_bytes = ((uint64_t{width} + layout.width - 1) / layout.width) * layout.bytes;
    const uint32_t rows = static_cast<uint32_t>((uint64_t{height} + layout.height - 1) / layout.height);
    D3DLOCKED_RECT lr{}; D3DLOCKED_BOX lb{};
    if (volume) {
        hr = object.kind == ObjectKind::Volume
            ? static_cast<IDirect3DVolume9 *>(object.native)->LockBox(&lb, box_ptr, flags)
            : static_cast<IDirect3DVolumeTexture9 *>(object.native)->LockBox(level, &lb, box_ptr, flags);
    } else if (object.kind == ObjectKind::Surface)
        hr = static_cast<IDirect3DSurface9 *>(object.native)->LockRect(&lr, rect_ptr, flags);
    else if (object.kind == ObjectKind::Texture)
        hr = static_cast<IDirect3DTexture9 *>(object.native)->LockRect(level, &lr, rect_ptr, flags);
    else hr = static_cast<IDirect3DCubeTexture9 *>(object.native)->LockRect(static_cast<D3DCUBEMAP_FACES>(face), level, &lr, rect_ptr, flags);
    if (FAILED(hr)) return hr;
    uint32_t allocation = 0;
    try {
        const int pitch = volume ? lb.RowPitch : lr.Pitch;
        const uint64_t slice = volume && lb.SlicePitch > 0 ? static_cast<uint32_t>(lb.SlicePitch) : uint64_t{static_cast<uint32_t>(pitch)} * rows;
        void *bits = volume ? lb.pBits : lr.pBits;
        const uint64_t size = slice * depth;
        if (!bits || pitch <= 0 || row_bytes > static_cast<uint32_t>(pitch) ||
            (volume && lb.SlicePitch <= 0) || slice < uint64_t{static_cast<uint32_t>(pitch)} * rows ||
            !size || size > UINT32_MAX || slice > UINT32_MAX)
            marshal_error("native lock returned null bits, invalid pitch, or a span exceeding the PE32 ABI");
        allocation = g_callbacks.guest_alloc(g_context, static_cast<uint32_t>(size), 4);
        if (!allocation) throw std::bad_alloc{};
        StagedLock lock{bits, allocation, static_cast<uint32_t>(size), static_cast<uint32_t>(pitch),
            static_cast<uint32_t>(slice), static_cast<uint32_t>(row_bytes), rows, depth, flags};
        std::memset(output<uint8_t>(allocation, size), 0, static_cast<size_t>(size));
        if (!(flags & D3DLOCK_DISCARD) && !copy_staged(lock, false)) marshal_error(g_last_error);
        g_staged_locks.emplace(key, lock);
        if (volume) {
            const GuestLockedBox guest{pitch,static_cast<int32_t>(slice),allocation};
            std::memcpy(out,&guest,sizeof(guest));
        } else {
            const GuestLockedRect guest{pitch,allocation};
            std::memcpy(out,&guest,sizeof(guest));
        }
    } catch (...) {
        if (allocation) g_callbacks.guest_free(g_context, allocation);
        unlock_staged_native(object, key);
        throw;
    }
    return hr;
}

bool is_resource(ObjectKind kind)
{
    return kind == ObjectKind::Surface || kind == ObjectKind::Texture ||
        kind == ObjectKind::CubeTexture || kind == ObjectKind::VolumeTexture ||
        kind == ObjectKind::VertexBuffer || kind == ObjectKind::IndexBuffer;
}

// Container IUnknown/BaseTexture/Resource requests are probed for a concrete
// supported interface before wrapping, so all paths preserve proxy identity.
uint32_t wrap_container(IUnknown *native)
{
    auto existing = g_guest_by_native.find(native);
    if (existing != g_guest_by_native.end())
        return wrap_native(native, g_objects.at(existing->second).kind, true);
    struct Candidate { const GUID *iid; ObjectKind kind; };
    const Candidate candidates[] = {
        {&kIidIDirect3DTexture9,ObjectKind::Texture}, {&kIidIDirect3DCubeTexture9,ObjectKind::CubeTexture},
        {&kIidIDirect3DVolumeTexture9,ObjectKind::VolumeTexture}, {&kIidIDirect3DSwapChain9,ObjectKind::SwapChain},
        {&kIidIDirect3DDevice9,ObjectKind::Device}, {&kIidIDirect3D9,ObjectKind::D3D9},
        {&kIidIDirect3DSurface9,ObjectKind::Surface}, {&kIidIDirect3DVolume9,ObjectKind::Volume},
        {&kIidIDirect3DVertexBuffer9,ObjectKind::VertexBuffer}, {&kIidIDirect3DIndexBuffer9,ObjectKind::IndexBuffer}
    };
    for (const auto &candidate : candidates) {
        void *concrete = nullptr;
        if (SUCCEEDED(native->QueryInterface(*candidate.iid, &concrete)) && concrete) {
            native->Release();
            return wrap_native(static_cast<IUnknown *>(concrete), candidate.kind, true);
        }
    }
    native->Release();
    unsupported("GetContainer returned a native interface with no supported guest proxy kind");
}

template <typename T>
uint32_t private_data(T *native, uint32_t method, const uint32_t *a)
{
    GUID guid{}; std::memcpy(&guid,input<GUID>(a[1]),sizeof(guid));
    const std::string key(reinterpret_cast<const char *>(&guid), sizeof(guid));
    IUnknown *identity = native;
    auto &records = g_private_interfaces[identity];
    if (method == 4) {
        const void *data;
        uint32_t size = a[3];
        ObjectKind kind = ObjectKind::D3D9;
        if (a[4] & D3DSPD_IUNKNOWN) {
            const auto it = g_objects.find(a[2]);
            if (size != 4 || it == g_objects.end()) marshal_error("SetPrivateData IUNKNOWN requires a guest proxy and PE32 pointer size 4");
            data = it->second.native; size = sizeof(IUnknown *); kind = it->second.kind;
        } else data = input<uint8_t>(a[2], size);
        const HRESULT hr = native->SetPrivateData(guid, data, size, a[4]);
        if (SUCCEEDED(hr)) {
            if (a[4] & D3DSPD_IUNKNOWN) records[key] = kind;
            else records.erase(key);
        }
        return hresult(hr);
    }
    if (method == 6) {
        const HRESULT hr = native->FreePrivateData(guid);
        if (SUCCEEDED(hr)) records.erase(key);
        return hresult(hr);
    }
    const DWORD requested = *input<DWORD>(a[3]);
    auto *size_out = output<DWORD>(a[3]);
    const auto record = records.find(key);
    if (record != records.end()) {
        *size_out = 4;
        if (!a[2] || requested < 4) return hresult(D3DERR_MOREDATA);
        auto *out = output<uint32_t>(a[2]);
        DWORD size = sizeof(IUnknown *); IUnknown *value = nullptr;
        const HRESULT hr = native->GetPrivateData(guid, &value, &size);
        if (SUCCEEDED(hr)) {
            *out = wrap_native(value, record->second, true);
            if (value && !*out) return hresult(E_OUTOFMEMORY);
        }
        return hresult(hr);
    }
    void *data = a[2] ? output<uint8_t>(a[2], requested) : nullptr;
    DWORD size = requested;
    const HRESULT hr = native->GetPrivateData(guid, data, &size);
    *size_out = size;
    return hresult(hr);
}

bool invoke_resource_extension(GuestObject &object, uint32_t method,
                               const uint32_t *a, uint32_t *result)
{
    auto finish = [&](HRESULT status) { *result = hresult(status); return true; };
    if (method == 3 && object.kind != ObjectKind::Device && object.kind != ObjectKind::D3D9 &&
        object.kind != ObjectKind::ConstantTable && object.kind != ObjectKind::SwapChain) {
        return finish(interface_output<IDirect3DDevice9>(a[1], ObjectKind::Device, [&](auto **out) {
            switch (object.kind) {
            case ObjectKind::VertexDeclaration: return static_cast<IDirect3DVertexDeclaration9 *>(object.native)->GetDevice(out);
            case ObjectKind::PixelShader: return static_cast<IDirect3DPixelShader9 *>(object.native)->GetDevice(out);
            case ObjectKind::VertexShader: return static_cast<IDirect3DVertexShader9 *>(object.native)->GetDevice(out);
            case ObjectKind::Query: return static_cast<IDirect3DQuery9 *>(object.native)->GetDevice(out);
            case ObjectKind::Volume: return static_cast<IDirect3DVolume9 *>(object.native)->GetDevice(out);
            case ObjectKind::StateBlock: return static_cast<IDirect3DStateBlock9 *>(object.native)->GetDevice(out);
            default: return static_cast<IDirect3DResource9 *>(object.native)->GetDevice(out);
            }
        }));
    }
    if (is_resource(object.kind) || object.kind == ObjectKind::Volume) {
        if (method >= 4 && method <= 6) {
            *result = object.kind == ObjectKind::Volume ? private_data(static_cast<IDirect3DVolume9 *>(object.native), method, a)
                : private_data(static_cast<IDirect3DResource9 *>(object.native), method, a);
            return true;
        }
        if (object.kind != ObjectKind::Volume && method >= 7 && method <= 10) {
            auto *resource = static_cast<IDirect3DResource9 *>(object.native);
            switch (method) {
            case 7: *result = resource->SetPriority(a[1]); break;
            case 8: *result = resource->GetPriority(); break;
            case 9: resource->PreLoad(); *result = 0; break;
            case 10: *result = static_cast<uint32_t>(resource->GetType()); break;
            }
            return true;
        }
    }
    if ((object.kind == ObjectKind::Surface && method == 11) ||
        (object.kind == ObjectKind::Volume && method == 7)) {
        GUID guid{}; std::memcpy(&guid,input<GUID>(a[1]),sizeof(guid));
        auto *out = output<uint32_t>(a[2]); *out = 0;
        // An arbitrary provider extension cannot escape as a native pointer.
        bool supported = false;
        for (uint32_t kind = 1; kind <= kSwapChainKind; ++kind)
            if (kind != kConstantTableKind && bridge_supports_iid(static_cast<ObjectKind>(kind), guid)) supported = true;
        if (!supported) unsupported("GetContainer requested an IID with no supported guest proxy");
        void *native = nullptr;
        const HRESULT hr = object.kind == ObjectKind::Surface
            ? static_cast<IDirect3DSurface9 *>(object.native)->GetContainer(guid, &native)
            : static_cast<IDirect3DVolume9 *>(object.native)->GetContainer(guid, &native);
        if (SUCCEEDED(hr) && native) { *out = wrap_container(static_cast<IUnknown *>(native)); if (!*out) return finish(E_OUTOFMEMORY); }
        else if (native) static_cast<IUnknown *>(native)->Release();
        return finish(hr);
    }
    if (object.kind == ObjectKind::Surface) {
        if (method == 13) return finish(lock_staged(object,a[0],0,0,a[1],a[2],a[3],false));
        if (method == 14) return finish(unlock_staged(object,{a[0],0,0}));
        if (method == 15 || method == 16) unsupported("IDirect3DSurface9::GetDC/ReleaseDC requires a guest HDC mapping callback; none is defined in ac1_d3d9_callbacks");
    }
    if ((object.kind == ObjectKind::VertexBuffer || object.kind == ObjectKind::IndexBuffer) && method == 13) {
        return finish(object.kind == ObjectKind::VertexBuffer
            ? copied_output<D3DVERTEXBUFFER_DESC>(a[1],[&](auto *p) { return static_cast<IDirect3DVertexBuffer9 *>(object.native)->GetDesc(p); })
            : copied_output<D3DINDEXBUFFER_DESC>(a[1],[&](auto *p) { return static_cast<IDirect3DIndexBuffer9 *>(object.native)->GetDesc(p); }));
    }
    if ((object.kind == ObjectKind::PixelShader || object.kind == ObjectKind::VertexShader) && method == 4) {
        UINT size = *input<UINT>(a[2]); auto *size_out = output<UINT>(a[2]);
        void *data = a[1] ? output<uint8_t>(a[1], size) : nullptr;
        const HRESULT hr = object.kind == ObjectKind::PixelShader
            ? static_cast<IDirect3DPixelShader9 *>(object.native)->GetFunction(data, &size)
            : static_cast<IDirect3DVertexShader9 *>(object.native)->GetFunction(data, &size);
        *size_out = size; return finish(hr);
    }
    if (object.kind == ObjectKind::Texture || object.kind == ObjectKind::CubeTexture || object.kind == ObjectKind::VolumeTexture) {
        auto *base = static_cast<IDirect3DBaseTexture9 *>(object.native);
        switch (method) {
        case 11: *result = base->SetLOD(a[1]); return true;
        case 12: *result = base->GetLOD(); return true;
        case 13: *result = base->GetLevelCount(); return true;
        case 14: return finish(base->SetAutoGenFilterType(static_cast<D3DTEXTUREFILTERTYPE>(a[1])));
        case 15: *result = static_cast<uint32_t>(base->GetAutoGenFilterType()); return true;
        case 16: base->GenerateMipSubLevels(); *result = 0; return true;
        }
        if (object.kind == ObjectKind::Texture) {
            auto *texture = static_cast<IDirect3DTexture9 *>(object.native);
            if (method == 17) return finish(copied_output<D3DSURFACE_DESC>(a[2],[&](auto *p) { return texture->GetLevelDesc(a[1],p); }));
            if (method == 18) return finish(interface_output<IDirect3DSurface9>(a[2],ObjectKind::Surface,[&](auto **out) { return texture->GetSurfaceLevel(a[1],out); }));
            if (method == 19) return finish(lock_staged(object,a[0],0,a[1],a[2],a[3],a[4],false));
            if (method == 20) return finish(unlock_staged(object,{a[0],0,a[1]}));
            if (method == 21) return finish(texture->AddDirtyRect(input<RECT>(a[1],1,true)));
        } else if (object.kind == ObjectKind::CubeTexture) {
            auto *texture = static_cast<IDirect3DCubeTexture9 *>(object.native);
            if (method == 17) return finish(copied_output<D3DSURFACE_DESC>(a[2],[&](auto *p) { return texture->GetLevelDesc(a[1],p); }));
            if (method == 18) return finish(interface_output<IDirect3DSurface9>(a[3],ObjectKind::Surface,[&](auto **out) { return texture->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(a[1]),a[2],out); }));
            if (method == 19) return finish(lock_staged(object,a[0],a[1],a[2],a[3],a[4],a[5],false));
            if (method == 20) return finish(unlock_staged(object,{a[0],a[1],a[2]}));
            if (method == 21) return finish(texture->AddDirtyRect(static_cast<D3DCUBEMAP_FACES>(a[1]),input<RECT>(a[2],1,true)));
        } else {
            auto *texture = static_cast<IDirect3DVolumeTexture9 *>(object.native);
            if (method == 17) return finish(copied_output<D3DVOLUME_DESC>(a[2],[&](auto *p) { return texture->GetLevelDesc(a[1],p); }));
            if (method == 18) return finish(interface_output<IDirect3DVolume9>(a[2],ObjectKind::Volume,[&](auto **out) { return texture->GetVolumeLevel(a[1],out); }));
            if (method == 19) return finish(lock_staged(object,a[0],0,a[1],a[2],a[3],a[4],true));
            if (method == 20) return finish(unlock_staged(object,{a[0],0,a[1]}));
            if (method == 21) return finish(texture->AddDirtyBox(input<D3DBOX>(a[1],1,true)));
        }
    }
    if (object.kind == ObjectKind::Volume) {
        auto *volume = static_cast<IDirect3DVolume9 *>(object.native);
        if (method == 8) return finish(copied_output<D3DVOLUME_DESC>(a[1],[&](auto *p) { return volume->GetDesc(p); }));
        if (method == 9) return finish(lock_staged(object,a[0],0,0,a[1],a[2],a[3],true));
        if (method == 10) return finish(unlock_staged(object,{a[0],0,0}));
    }
    if (object.kind == ObjectKind::StateBlock) {
        auto *block = static_cast<IDirect3DStateBlock9 *>(object.native);
        if (method == 4) return finish(block->Capture());
        if (method == 5) return finish(block->Apply());
    }
    if (object.kind == ObjectKind::SwapChain) {
        auto *swap = static_cast<IDirect3DSwapChain9 *>(object.native);
        switch (method) {
        case 3: { const HRESULT hr = present_device(nullptr,a,swap,a[5]); if (SUCCEEDED(hr)) trace_frame_success(true,"SwapChain::Present"); return finish(hr); }
        case 4: return finish(swap->GetFrontBufferData(native_object<IDirect3DSurface9>(a[1],ObjectKind::Surface)));
        case 5: return finish(interface_output<IDirect3DSurface9>(a[3],ObjectKind::Surface,[&](auto **out) { return swap->GetBackBuffer(a[1],static_cast<D3DBACKBUFFER_TYPE>(a[2]),out); }));
        case 6: return finish(copied_output<D3DRASTER_STATUS>(a[1],[&](auto *p) { return swap->GetRasterStatus(p); }));
        case 7: return finish(copied_output<D3DDISPLAYMODE>(a[1],[&](auto *p) { return swap->GetDisplayMode(p); }));
        case 8: return finish(interface_output<IDirect3DDevice9>(a[1],ObjectKind::Device,[&](auto **out) { return swap->GetDevice(out); }));
        case 9: {
            auto *out = output<GuestPresentParameters>(a[1]); D3DPRESENT_PARAMETERS host{};
            const HRESULT hr = swap->GetPresentParameters(&host);
            if (SUCCEEDED(hr)) {
                bool ok;
                const auto hwnd = host_window(object.guest_window,&ok);
                if (!ok || reinterpret_cast<uintptr_t>(host.hDeviceWindow) != hwnd)
                    unsupported("GetPresentParameters cannot map native HWND to a guest handle");
                *out = guest_present_parameters(host,object.guest_window);
                if (object.forced_windowed) { out->windowed = FALSE; out->fullscreen_refresh_rate = object.fullscreen_refresh; }
            }
            return finish(hr);
        }
        }
    }
    return false;
}

uint32_t invoke_unknown(GuestObject &object, uint32_t method, const uint32_t *a)
{
    if (method == 0) { uint32_t value = 0; return hresult(do_query_interface(object,a,&value)); }
    if (method == 1) { object.native->AddRef(); return ++object.guest_references; }
    const uint32_t refs = object.guest_references - 1;
    if (!refs) {
        discard_staged_locks(a[0],object);
        if (object.kind == ObjectKind::VertexBuffer || object.kind == ObjectKind::IndexBuffer) abandon_buffer_lock(object);
        if (object.kind == ObjectKind::ConstantTable) release_constant_table_allocations(a[0]);
    }
    IUnknown *native = object.native;
    const ULONG native_refs = native->Release();
    object.guest_references = refs;
    if (!native_refs) g_private_interfaces.erase(native);
    if (!refs) {
        const GuestObject retired = object;
        g_guest_by_native.erase(native); g_objects.erase(a[0]); free_guest_object(a[0],retired);
    }
    return refs;
}

} // namespace

extern "C" AC1_D3D9_API int ac1_d3d9_configure(
    const ac1_d3d9_callbacks *callbacks, void *context)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    clear_error();
    if (!callbacks || callbacks->struct_size < sizeof(ac1_d3d9_callbacks) ||
        !callbacks->guest_resolve || !callbacks->guest_alloc || !callbacks->guest_free) {
        set_error("callback table is incomplete or has an incompatible struct_size");
        return 0;
    }
    if (!g_objects.empty()) {
        set_error("cannot replace guest callbacks while COM objects are live");
        return 0;
    }
    g_callbacks = *callbacks;
    g_context = context;
    return 1;
}

extern "C" AC1_D3D9_API uint32_t ac1_d3d9_create9(uint32_t sdk_version)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    clear_error();
    if (!g_callbacks.guest_resolve || !g_callbacks.guest_alloc || !g_callbacks.guest_free) {
        set_error("configure guest memory callbacks before Direct3DCreate9");
        return 0;
    }

    IDirect3D9 *native = nullptr;
    if (g_callbacks.factory) {
        native = static_cast<IDirect3D9 *>(g_callbacks.factory(g_context, sdk_version));
    } else {
#if defined(_WIN32)
        if (!ensure_d3d9_module()) return 0;
        using Factory = IDirect3D9 *(WINAPI *)(UINT);
        FARPROC raw_factory = GetProcAddress(g_d3d9_module, "Direct3DCreate9");
        static_assert(sizeof(Factory) == sizeof(raw_factory),
                      "Windows function-pointer ABI mismatch");
        Factory factory = nullptr;
        std::memcpy(&factory, &raw_factory, sizeof(factory));
        if (!factory) {
            set_error("selected D3D9 provider has no Direct3DCreate9 export: " +
                      g_d3d9_provider_path);
            return 0;
        }
        native = factory(sdk_version);
#else
        set_error("no platform D3D9 factory was supplied");
        return 0;
#endif
    }
    if (!native) {
        set_error("Direct3DCreate9 returned null from provider: " +
                  (g_d3d9_provider_path.empty() ? std::string("platform callback")
                                                : g_d3d9_provider_path));
        return 0;
    }
    uint32_t address = wrap_native(native, ObjectKind::D3D9, true);
    if (!address) set_error("failed to wrap Direct3DCreate9 result");
    return address;
}

extern "C" AC1_D3D9_API int ac1_d3d9_check_texture_requirements(
    uint32_t guest_device, uint32_t guest_width, uint32_t guest_height,
    uint32_t guest_mip_levels, uint32_t usage, uint32_t guest_format,
    uint32_t pool, uint32_t *native_hresult_out)
{
    if (!native_hresult_out) {
        set_error("D3DXCheckTextureRequirements bridge result pointer is null");
        return -1;
    }
    *native_hresult_out = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    clear_error();
    GuestObject *device_object = find_object(guest_device, ObjectKind::Device);
    if (!device_object) {
        set_error("D3DXCheckTextureRequirements received an unknown guest D3D9 device");
        return -1;
    }
#if defined(_WIN32)
    if (!ensure_d3dx9_module()) return -2;
#endif

    const uint32_t guest_addresses[4] = {
        guest_width, guest_height, guest_mip_levels, guest_format
    };
    uint32_t staged_values[4]{};
    uint32_t *native_values[4]{};
    for (size_t i = 0; i < 4; ++i) {
        const uint32_t address = guest_addresses[i];
        if (!address) continue;
        size_t alias = i;
        for (size_t j = 0; j < i; ++j) {
            if (guest_addresses[j] == address) {
                alias = j;
                break;
            }
        }
        if (alias != i) {
            native_values[i] = native_values[alias];
            continue;
        }
        if (!guest_read(address, &staged_values[i], sizeof(uint32_t)) ||
            !guest_memory(address, sizeof(uint32_t), AC1_D3D9_GUEST_WRITE)) {
            set_error("D3DXCheckTextureRequirements guest in/out pointer is unmapped");
            return -1;
        }
        native_values[i] = &staged_values[i];
    }

    const HRESULT status =
#if defined(_WIN32)
        g_d3dx_check_texture_requirements(
#else
        ac1_d3dx::check_texture_requirements(
#endif
        static_cast<IDirect3DDevice9 *>(device_object->native),
        reinterpret_cast<UINT *>(native_values[0]),
        reinterpret_cast<UINT *>(native_values[1]),
        reinterpret_cast<UINT *>(native_values[2]), usage,
        reinterpret_cast<D3DFORMAT *>(native_values[3]), static_cast<D3DPOOL>(pool));
#if !defined(_WIN32)
    if (!ac1_d3dx::diagnostic.empty()) set_error(ac1_d3dx::diagnostic);
#endif
    for (size_t i = 0; i < 4; ++i) {
        if (!guest_addresses[i]) continue;
        bool already_written = false;
        for (size_t j = 0; j < i; ++j) {
            if (guest_addresses[j] == guest_addresses[i]) {
                already_written = true;
                break;
            }
        }
        if (!already_written &&
            !guest_write(guest_addresses[i], &staged_values[i], sizeof(uint32_t))) {
            set_error("D3DXCheckTextureRequirements could not write an in/out value back to guest memory");
            return -1;
        }
    }
    *native_hresult_out = hresult(status);
    return 1;
}

extern "C" AC1_D3D9_API int ac1_d3d9_get_shader_constant_table(
    uint32_t guest_bytecode, uint32_t guest_output,
    uint32_t *native_hresult_out)
{
    if (!native_hresult_out) {
        set_error("D3DXGetShaderConstantTable bridge result pointer is null");
        return -1;
    }
    *native_hresult_out = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    clear_error();
    if (!guest_output ||
        !guest_memory(guest_output, sizeof(uint32_t), AC1_D3D9_GUEST_WRITE)) {
        set_error("D3DXGetShaderConstantTable output pointer is null or unmapped");
        return -1;
    }
    const uint32_t zero = 0;
    if (!guest_write(guest_output, &zero, sizeof(zero))) {
        set_error("D3DXGetShaderConstantTable output pointer could not be initialized");
        return -1;
    }

    std::vector<DWORD> bytecode;
    const int scan_status = scan_shader_for_d3dx(guest_bytecode, &bytecode);
    if (scan_status != 1) return scan_status;
#if defined(_WIN32)
    if (!ensure_d3dx_shader_table_export()) return -2;
#endif

    ID3DXConstantTable *native_table = nullptr;
#if defined(_WIN32)
    const HRESULT status = g_d3dx_get_shader_constant_table(
        bytecode.data(), &native_table);
#else
    const HRESULT status = ac1_d3dx::Table::create(bytecode.data(), bytecode.size(), &native_table);
    if (!ac1_d3dx::diagnostic.empty()) set_error(ac1_d3dx::diagnostic);
#endif
    if (FAILED(status)) {
        if (native_table) native_table->Release();
        *native_hresult_out = hresult(status);
        return 1;
    }
    if (!native_table) {
        set_error("native D3DXGetShaderConstantTable succeeded without a table object");
        *native_hresult_out = hresult(E_FAIL);
        return 1;
    }
    const uint32_t guest_table = wrap_native(
        static_cast<IUnknown *>(native_table), ObjectKind::ConstantTable, true);
    if (!guest_table) {
        if (!g_last_error[0]) set_error("failed to wrap native D3DX constant table");
        *native_hresult_out = hresult(E_OUTOFMEMORY);
        return 1;
    }
    if (!guest_write(guest_output, &guest_table, sizeof(guest_table))) {
        GuestObject *wrapped = find_object(guest_table, ObjectKind::ConstantTable);
        if (wrapped) {
            wrapped->native->Release();
            GuestObject retired = *wrapped;
            g_guest_by_native.erase(retired.native);
            g_objects.erase(guest_table);
            free_guest_object(guest_table, retired);
        }
        *native_hresult_out = hresult(D3DERR_INVALIDCALL);
        set_error("D3DXGetShaderConstantTable could not write its guest proxy output");
        return -1;
    }
    *native_hresult_out = hresult(status);
    return 1;
}

extern "C" AC1_D3D9_API int ac1_d3d9_dispatch(uint32_t token,
                                               const uint32_t *args,
                                               uint32_t arg_count,
                                               uint32_t *result_out)
{
    const int expected_arguments = token_argument_count(token);
    if (expected_arguments == 0) return 0;
    if (expected_arguments == -2) {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::ostringstream message;
        message << "COM token 0x" << std::hex << token << " names an unsupported method";
        set_error(message.str());
        return -2;
    }
    if (expected_arguments < 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        set_error("malformed D3D9 method token or slot");
        return -1;
    }
    if (!args || !result_out || arg_count != static_cast<uint32_t>(expected_arguments)) {
        set_error("invalid COM dispatch arguments");
        return -1;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    clear_error();
    const uint32_t offset = token - kTokenBase;
    const uint32_t kind = offset / kKindStride;
    const uint32_t within_kind = offset % kKindStride;
    const uint32_t method = within_kind / kSlotStride;
    GuestObject *object = find_object(args[0], static_cast<ObjectKind>(kind));
    if (!object) {
        set_error("COM token called with an unknown guest object or interface kind");
        return -1;
    }
    {
        /* AC1_D3D9_TRACE_CALLS_AT=N: log every COM call of the frame that ends
         * with the N-th device Present (1-based), then the Present itself. */
        static long long trace_at = -1, frame = 1;
        if (trace_at < 0) {
            const char *value = std::getenv("AC1_D3D9_TRACE_CALLS_AT");
            trace_at = value ? std::atoll(value) : 0;
        }
        if (trace_at > 0 && frame == trace_at) {
            std::fprintf(stderr, "AC1 D3D9 call frame=%lld kind=%u slot=%u args=", frame, kind, method);
            for (uint32_t i = 1; i < arg_count; ++i)
                std::fprintf(stderr, "%s%08x", i > 1 ? "," : "", args[i]);
            std::fprintf(stderr, "\n");
        }
        if (kind == kDeviceKind && method == 17) ++frame;
    }
    if ((object->kind == ObjectKind::VertexBuffer ||
         object->kind == ObjectKind::IndexBuffer) && method == 11) {
        if (object->buffer_lock_guest_address || object->buffer_lock_native_pointer) {
            set_error("D3D9 buffer Lock is unsupported while this proxy already has an outstanding lock");
            return -2;
        }
        if (!supports_guest_buffer_lock_flags(args[4])) {
            std::ostringstream message;
            message << "D3D9 buffer Lock flags 0x" << std::hex << args[4]
                    << " are outside the guest staging profile";
            set_error(message.str());
            return -2;
        }
    }
    if (object->kind == ObjectKind::Device && method == 65 && args[2] != 0u &&
        !find_object(args[2], ObjectKind::Texture) &&
        !find_object(args[2], ObjectKind::CubeTexture) &&
        !find_object(args[2], ObjectKind::VolumeTexture)) {
        set_error("SetTexture received an unregistered 2D/cube texture proxy");
        return -2;
    }
    if (object->kind == ObjectKind::Device && method == 39 && args[1] != 0u &&
        !find_object(args[1], ObjectKind::Surface)) {
        set_error("SetDepthStencilSurface received an unregistered surface proxy");
        return -2;
    }
    if (object->kind == ObjectKind::Device && method == 37 && args[2] != 0u &&
        !find_object(args[2], ObjectKind::Surface)) {
        set_error("SetRenderTarget received an unregistered surface proxy");
        return -2;
    }
    if (object->kind == ObjectKind::Device && method == 87 && args[1] != 0u &&
        !find_object(args[1], ObjectKind::VertexDeclaration)) {
        set_error("SetVertexDeclaration received an unregistered declaration proxy");
        return -2;
    }
    if (object->kind == ObjectKind::Device && method == 100 && args[2] != 0u &&
        !find_object(args[2], ObjectKind::VertexBuffer)) {
        set_error("SetStreamSource received an unregistered vertex-buffer proxy");
        return -2;
    }
    if (object->kind == ObjectKind::Device && method == 104 && args[1] != 0u &&
        !find_object(args[1], ObjectKind::IndexBuffer)) {
        set_error("SetIndices received an unregistered index-buffer proxy");
        return -2;
    }
    if (object->kind == ObjectKind::Device && method == 25 && args[7] != 0u) {
        set_error("IDirect3DDevice9::CreateCubeTexture shared handles are unsupported");
        return -2;
    }
    if (object->kind == ObjectKind::Device && method == 17 && args[4] != 0u) {
        RGNDATAHEADER header{};
        if (guest_read(args[4], &header, sizeof(header)) &&
            (header.iType != kRdhRectangles ||
             header.nRgnSize > 16u * 1024u * 1024u - sizeof(RGNDATAHEADER))) {
            set_error("IDirect3DDevice9::Present dirty-region type/size is outside the supported profile");
            return -2;
        }
    }
    if (object->kind == ObjectKind::Device && method == 92 && args[1] != 0u &&
        !find_object(args[1], ObjectKind::VertexShader)) {
        set_error("SetVertexShader received an unregistered guest shader object");
        return -2;
    }
    if (object->kind == ObjectKind::Device && method == 107 && args[1] != 0u &&
        !find_object(args[1], ObjectKind::PixelShader)) {
        set_error("SetPixelShader received an unregistered guest shader object");
        return -2;
    }
    if (object->kind == ObjectKind::Device &&
        (method == 26 || method == 27) && args[6] != 0u) {
        set_error("D3D9 shared buffer handles are unsupported by the guest bridge");
        return -2;
    }
    if (object->kind == ObjectKind::Device && method == 23 && args[8] != 0u) {
        set_error("D3D9 shared texture handles are unsupported by the guest bridge");
        return -2;
    }
    if (method == 0) {
        GUID iid{};
        if (guest_read(args[1], &iid, sizeof(iid)) &&
            !bridge_supports_iid(object->kind, iid)) {
            report_unsupported_iid(object->kind, iid);
            return -2;
        }
    }
    try {
    if (method <= 2) { *result_out = invoke_unknown(*object,method,args); return 1; }
    if (invoke_resource_extension(*object,method,args,result_out)) return 1;
    switch (object->kind) {
    case ObjectKind::D3D9:
        *result_out = invoke_d3d9(*object, method, args, arg_count);
        break;
    case ObjectKind::Device:
        *result_out = invoke_device(*object, method, args, arg_count);
        break;
    case ObjectKind::Surface:
        *result_out = hresult(copied_output<D3DSURFACE_DESC>(args[1],[&](auto *p) { return static_cast<IDirect3DSurface9 *>(object->native)->GetDesc(p); }));
        break;
    case ObjectKind::VertexBuffer:
    case ObjectKind::IndexBuffer:
        *result_out = invoke_buffer(*object, method, args, arg_count);
        break;
    case ObjectKind::VertexDeclaration:
        *result_out = invoke_vertex_declaration(*object, method, args, arg_count);
        break;

    case ObjectKind::ConstantTable:
        *result_out = invoke_constant_table(*object, method, args, arg_count);
        break;
    case ObjectKind::Texture:
    case ObjectKind::CubeTexture:
    case ObjectKind::PixelShader:
    case ObjectKind::VertexShader:
    case ObjectKind::VolumeTexture:
    case ObjectKind::Volume:
    case ObjectKind::StateBlock:
    case ObjectKind::SwapChain:
        marshal_error("unrecognized resource interface slot " + std::to_string(method));
    case ObjectKind::Query:
        *result_out = invoke_query(*object, method, args, arg_count);
        break;
    }
    } catch (const UnsupportedMarshal &) {
        const std::string detail = g_last_error;
        set_error(std::string(interface_name(object->kind)) + " slot " + std::to_string(method) + ": " + detail);
        return -2;
    } catch (const MarshalError &) {
        const std::string detail = g_last_error;
        set_error(std::string(interface_name(object->kind)) + " slot " +
                  std::to_string(method) + ": " + detail);
        return -1;
    } catch (const std::bad_alloc &) {
        set_error("D3D9 bridge staging allocation failed");
        *result_out = hresult(E_OUTOFMEMORY);
        return 1;
    }
    return 1;
}

extern "C" AC1_D3D9_API int ac1_d3d9_argument_count(uint32_t token)
{
    return token_argument_count(token);
}

extern "C" AC1_D3D9_API void ac1_d3d9_shutdown(void)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto &entry : g_objects) {
        discard_staged_locks(entry.first,entry.second);
        if (entry.second.kind == ObjectKind::VertexBuffer ||
            entry.second.kind == ObjectKind::IndexBuffer)
            abandon_buffer_lock(entry.second);
    }
    for (auto &entry : g_objects) {
        if (entry.second.kind == ObjectKind::ConstantTable)
            release_constant_table_allocations(entry.first);
    }
    for (auto &entry : g_objects) {
        GuestObject &object = entry.second;
        for (uint32_t refs = object.guest_references; refs; --refs)
            object.native->Release();
        free_guest_object(entry.first, object);
    }
    g_objects.clear();
    g_staged_locks.clear();
    g_private_interfaces.clear();
    g_guest_by_native.clear();
    g_constant_handles.clear();
    g_guest_handle_by_native.clear();
    g_constant_table_allocations.clear();
    g_successful_draw_calls = 0;
    g_successful_present_calls = 0;
    g_frame_trace_enabled = false;
    g_frame_trace_initialized = false;
    g_first_draw_reported = false;
    g_first_present_reported = false;
#if defined(_WIN32)
    if (g_d3d9_module) {
        FreeLibrary(g_d3d9_module);
        g_d3d9_module = nullptr;
    }
    if (g_d3dx9_module) {
        FreeLibrary(g_d3dx9_module);
        g_d3dx9_module = nullptr;
        g_d3dx_check_texture_requirements = nullptr;
        g_d3dx_get_shader_constant_table = nullptr;
    }
#endif
    g_d3d9_provider_path.clear();
}

extern "C" AC1_D3D9_API const char *ac1_d3d9_last_error(void)
{
    return g_last_error;
}

extern "C" AC1_D3D9_API uint64_t ac1_d3d9_present_count(void)
{
    return g_successful_present_calls.load(std::memory_order_relaxed);
}
