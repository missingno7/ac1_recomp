/*
 * AC1 guest-to-host D3D9 COM ABI bridge.
 *
 * Guest object/vtable tokens follow the general COM-token technique in
 * codepdbh/nfsu2-recompiled's Android D3D9 bridge (MIT; copyright codepdbh).
 * This implementation is a separate AC1 adapter: no NFS engine code, guest
 * addresses, graphics policy, or game-specific compatibility behavior is used.
 */

#include "ac1_d3d9_bridge.h"

#include <d3d9.h>
#if defined(_WIN32)
#include <windows.h>
#include <d3dx9shader.h>
#endif

#include <algorithm>
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
constexpr uint32_t kD3D9VtableSlots = 17u;
constexpr uint32_t kDeviceVtableSlots = 119u;
constexpr uint32_t kSurfaceVtableSlots = 17u;
constexpr uint32_t kPixelShaderVtableSlots = 5u;
constexpr uint32_t kVertexShaderVtableSlots = 5u;
constexpr uint32_t kConstantTableVtableSlots = 27u;
constexpr uint32_t kCubeTextureVtableSlots = 22u;

constexpr GUID kIidIUnknown =
    {0x00000000u, 0x0000u, 0x0000u, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIidIDirect3D9 =
    {0x81bdcbcau, 0x64d4u, 0x426du, {0xae, 0x8d, 0xad, 0x01, 0x47, 0xf4, 0x27, 0x5c}};
constexpr GUID kIidIDirect3DDevice9 =
    {0xd0223b96u, 0xbf7au, 0x43fdu, {0x92, 0xbd, 0xa4, 0x3b, 0x0d, 0x82, 0xb9, 0xeb}};
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
    CubeTexture = kCubeTextureKind
};

struct GuestObject {
    IUnknown *native = nullptr;
    ObjectKind kind = ObjectKind::D3D9;
    uint32_t guest_vtable = 0;
    uint32_t guest_references = 1;
};

#pragma pack(push, 4)
struct GuestLockedRect {
    int32_t pitch;
    uint32_t bits;
};
#pragma pack(pop)
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

struct TextureLock {
    void *native_bits = nullptr;
    uint32_t guest_bits = 0;
    uint32_t allocation_size = 0;
    uint32_t pitch = 0;
    uint32_t row_bytes = 0;
    uint32_t rows = 0;
    uint32_t face = 0;
    uint32_t level = 0;
};

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
static_assert(sizeof(D3DVERTEXELEMENT9) == 8,
              "guest D3DVERTEXELEMENT9 must remain 8 bytes");

ac1_d3d9_callbacks g_callbacks{};
void *g_context = nullptr;
std::map<uint32_t, GuestObject> g_objects;
std::map<IUnknown *, uint32_t> g_guest_by_native;
std::map<TextureLockKey, TextureLock> g_texture_locks;
struct GuestConstantHandle {
    uint32_t table = 0;
    const char *native = nullptr;
};
std::map<uint32_t, std::vector<uint32_t>> g_constant_table_allocations;
std::map<uint32_t, GuestConstantHandle> g_constant_handles;
std::map<std::pair<uint32_t, uintptr_t>, uint32_t> g_guest_handle_by_native;
std::mutex g_mutex;
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

TextureLockKey texture_lock_key(uint32_t guest_texture, uint32_t face,
                                uint32_t level)
{
    return {guest_texture, face, level};
}

bool copyback_texture_lock(const TextureLock &lock)
{
    auto *staging = g_callbacks.guest_resolve
        ? static_cast<const uint8_t *>(g_callbacks.guest_resolve(
            g_context, lock.guest_bits, lock.allocation_size,
            AC1_D3D9_GUEST_READ)) : nullptr;
    if (!staging) {
        set_error("texture release could not read its guest lock staging buffer");
        return false;
    }
    for (uint32_t row = 0; row < lock.rows; ++row) {
        std::memcpy(static_cast<uint8_t *>(lock.native_bits) +
                        static_cast<size_t>(row) * lock.pitch,
                    staging + static_cast<size_t>(row) * lock.pitch,
                    lock.row_bytes);
    }
    return true;
}

void discard_texture_locks(uint32_t guest_texture, IDirect3DTexture9 *texture)
{
    auto it = g_texture_locks.lower_bound(texture_lock_key(guest_texture, 0, 0));
    while (it != g_texture_locks.end() &&
           it->first.guest_texture == guest_texture) {
        if (texture) {
            copyback_texture_lock(it->second);
            const HRESULT status = texture->UnlockRect(it->second.level);
            if (FAILED(status))
                set_error("texture release encountered a native UnlockRect failure");
        }
        if (g_callbacks.guest_free && it->second.guest_bits)
            g_callbacks.guest_free(g_context, it->second.guest_bits);
        it = g_texture_locks.erase(it);
    }
}

void discard_cube_texture_locks(uint32_t guest_texture,
                               IDirect3DCubeTexture9 *texture)
{
    auto it = g_texture_locks.lower_bound(texture_lock_key(guest_texture, 0, 0));
    while (it != g_texture_locks.end() &&
           it->first.guest_texture == guest_texture) {
        if (texture) {
            copyback_texture_lock(it->second);
            const HRESULT status = texture->UnlockRect(
                static_cast<D3DCUBEMAP_FACES>(it->second.face), it->second.level);
            if (FAILED(status))
                set_error("cube texture release encountered native UnlockRect failure");
        }
        if (g_callbacks.guest_free && it->second.guest_bits)
            g_callbacks.guest_free(g_context, it->second.guest_bits);
        it = g_texture_locks.erase(it);
    }
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
    }
    return 0;
}

int token_argument_count(uint32_t token)
{
    if (token < kTokenBase || token >= kTokenBase + 12 * kKindStride) return 0;
    const uint32_t offset = token - kTokenBase;
    const uint32_t kind = offset / kKindStride;
    const uint32_t within_kind = offset % kKindStride;
    if (kind < kD3D9Kind || kind > kCubeTextureKind) return 0;
    if (within_kind % kSlotStride) return -1;
    const uint32_t method = within_kind / kSlotStride;
    if (method >= slot_count(static_cast<ObjectKind>(kind))) return -1;

    if (kind == kDeviceKind) {
        if (method < 3) return method == 0 ? 3 : 1;
        if (method == 3) return 1;
        if (method == 35) return 4;
        if (method == 47) return 2;
        if (method == 86) return 3;
        if (method == 106) return 3;
        if (method == 91) return 3;
        if (method == 23) return 9;
        if (method == 25) return 8;
        if (method == 26 || method == 27) return 7;
        if (method == 65 || method == 92 || method == 107) return method == 65 ? 3 : 2;
        if (method == 67 || method == 69) return 4;
        if (method == 94 || method == 96 || method == 98 ||
            method == 109 || method == 111 || method == 113) return 4;
        if (method == 57) return 3;
        if (method == 38) return 3;
        if (method == 40) return 2;
        return -2;
    }
    if (kind == kSurfaceKind) {
        if (method < 3) return method == 0 ? 3 : 1;
        if (method == 3) return 2;
        return method == 12 ? 2 : -2;
    }
    if (kind == kVertexBufferKind || kind == kIndexBufferKind) {
        if (method < 3) return method == 0 ? 3 : 1;
        return -2;
    }
    if (kind == kVertexDeclarationKind) {
        if (method < 3) return method == 0 ? 3 : 1;
        if (method == 4) return 3;
        return -2;
    }
    if (kind == kTextureKind) {
        if (method < 3) return method == 0 ? 3 : 1;
        if (method == 19) return 5;
        if (method == 20) return 2;
        return -2;
    }
    if (kind == kCubeTextureKind) {
        if (method < 3) return method == 0 ? 3 : 1;
        if (method == 19) return 6;
        if (method == 20) return 3;
        return -2;
    }
    if (kind == kPixelShaderKind || kind == kVertexShaderKind) {
        if (method < 3) return method == 0 ? 3 : 1;
        return -2;
    }
    if (kind == kConstantTableKind) {
        if (method < 3) return method == 0 ? 3 : 1;
        if (method == 5) return 2;
        if (method == 6) return 4;
        if (method == 8) return 3;
        return -2;
    }

    /* IDirect3D9 slots 0..16. Values include the COM `this` stack word. */
    static constexpr int arities[kD3D9VtableSlots] = {
        3, 1, 1, -2, 1, 4, 3, 5, 3, 6, 7, 7, 6, 5, 4, 2, 7
    };
    return arities[method];
}

bool write_guest_word(uint32_t address, uint32_t value)
{
    return guest_write(address, &value, sizeof(value));
}

void free_guest_object(uint32_t address, const GuestObject &object)
{
    if (g_callbacks.guest_free) {
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
    case ObjectKind::PixelShader: supported = &kIidIDirect3DPixelShader9; break;
    case ObjectKind::VertexShader: supported = &kIidIDirect3DVertexShader9; break;
    case ObjectKind::ConstantTable: supported = &kIidID3DXConstantTable36; break;
    }
    return std::memcmp(&iid, &kIidIUnknown, sizeof(iid)) == 0 ||
           std::memcmp(&iid, supported, sizeof(iid)) == 0 ||
           ((kind == ObjectKind::Texture || kind == ObjectKind::CubeTexture) &&
            std::memcmp(&iid, &kIidIDirect3DBaseTexture9, sizeof(iid)) == 0);
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
    case ObjectKind::PixelShader: return "IDirect3DPixelShader9";
    case ObjectKind::VertexShader: return "IDirect3DVertexShader9";
    case ObjectKind::ConstantTable: return "ID3DXConstantTable";
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

HRESULT lock_texture_rect(uint32_t guest_texture, IDirect3DTexture9 *texture,
                          const uint32_t *args)
{
    if (!args[2]) {
        set_error("IDirect3DTexture9::LockRect output pointer is null");
        return D3DERR_INVALIDCALL;
    }
    auto *guest_output = static_cast<GuestLockedRect *>(guest_memory(
        args[2], sizeof(GuestLockedRect), AC1_D3D9_GUEST_WRITE));
    if (!guest_output) {
        set_error("IDirect3DTexture9::LockRect output structure is unmapped");
        return D3DERR_INVALIDCALL;
    }
    if (args[3] || args[4]) {
        set_error("texture LockRect RECT/flags profile is not implemented");
        return D3DERR_INVALIDCALL;
    }

    const TextureLockKey key = texture_lock_key(guest_texture, 0, args[1]);
    if (g_texture_locks.find(key) != g_texture_locks.end()) {
        set_error("IDirect3DTexture9::LockRect called twice for the same mip level");
        return D3DERR_INVALIDCALL;
    }

    D3DSURFACE_DESC description{};
    HRESULT status = texture->GetLevelDesc(args[1], &description);
    if (FAILED(status)) return status;
    if (description.Format != D3DFMT_A8R8G8B8) {
        set_error("texture LockRect supports only the observed A8R8G8B8 format");
        return D3DERR_NOTAVAILABLE;
    }
    const uint64_t row_bytes_64 = static_cast<uint64_t>(description.Width) * 4u;
    if (!description.Width || !description.Height || row_bytes_64 > UINT32_MAX) {
        set_error("texture LockRect dimensions exceed the guest staging ABI");
        return D3DERR_INVALIDCALL;
    }

    D3DLOCKED_RECT native{};
    status = texture->LockRect(args[1], &native, nullptr, 0);
    if (FAILED(status)) return status;
    if (!native.pBits || native.Pitch <= 0 ||
        static_cast<uint32_t>(native.Pitch) < row_bytes_64) {
        texture->UnlockRect(args[1]);
        set_error("native texture LockRect returned an unsupported pitch or null bits");
        return D3DERR_INVALIDCALL;
    }

    const uint64_t size_64 = static_cast<uint64_t>(native.Pitch) * description.Height;
    if (size_64 > UINT32_MAX || !size_64) {
        texture->UnlockRect(args[1]);
        set_error("native texture LockRect span overflows 32-bit guest memory");
        return D3DERR_INVALIDCALL;
    }
    const uint32_t allocation_size = static_cast<uint32_t>(size_64);
    const uint32_t guest_bits = g_callbacks.guest_alloc
        ? g_callbacks.guest_alloc(g_context, allocation_size, 4) : 0;
    if (!guest_bits) {
        texture->UnlockRect(args[1]);
        set_error("guest allocation failed for the locked texture staging buffer");
        return E_OUTOFMEMORY;
    }
    auto *staging = static_cast<uint8_t *>(guest_memory(
        guest_bits, allocation_size, AC1_D3D9_GUEST_WRITE));
    if (!staging) {
        g_callbacks.guest_free(g_context, guest_bits);
        texture->UnlockRect(args[1]);
        set_error("allocated texture staging buffer is not writable in guest memory");
        return D3DERR_INVALIDCALL;
    }
    std::memset(staging, 0, allocation_size);
    const uint32_t pitch = static_cast<uint32_t>(native.Pitch);
    const uint32_t row_bytes = static_cast<uint32_t>(row_bytes_64);
    for (uint32_t row = 0; row < description.Height; ++row) {
        std::memcpy(staging + static_cast<size_t>(row) * pitch,
                    static_cast<const uint8_t *>(native.pBits) +
                        static_cast<size_t>(row) * pitch,
                    row_bytes);
    }

    try {
        g_texture_locks.emplace(key, TextureLock{
            native.pBits, guest_bits, allocation_size, pitch, row_bytes,
            description.Height, 0, args[1]});
    } catch (...) {
        g_callbacks.guest_free(g_context, guest_bits);
        texture->UnlockRect(args[1]);
        set_error("failed to register the guest texture lock");
        return E_OUTOFMEMORY;
    }

    const GuestLockedRect locked{native.Pitch, guest_bits};
    std::memcpy(guest_output, &locked, sizeof(locked));
    return status;
}

HRESULT unlock_texture_rect(uint32_t guest_texture, IDirect3DTexture9 *texture,
                            uint32_t level)
{
    const TextureLockKey key = texture_lock_key(guest_texture, 0, level);
    auto it = g_texture_locks.find(key);
    if (it == g_texture_locks.end()) {
        set_error("IDirect3DTexture9::UnlockRect has no matching guest LockRect");
        return D3DERR_INVALIDCALL;
    }
    TextureLock &locked = it->second;
    auto *staging = static_cast<const uint8_t *>(guest_memory(
        locked.guest_bits, locked.allocation_size, AC1_D3D9_GUEST_READ));
    if (!staging) {
        set_error("locked texture staging buffer became unreadable in guest memory");
        return D3DERR_INVALIDCALL;
    }
    for (uint32_t row = 0; row < locked.rows; ++row) {
        std::memcpy(static_cast<uint8_t *>(locked.native_bits) +
                        static_cast<size_t>(row) * locked.pitch,
                    staging + static_cast<size_t>(row) * locked.pitch,
                    locked.row_bytes);
    }
    const HRESULT status = texture->UnlockRect(level);
    if (FAILED(status)) return status;
    if (g_callbacks.guest_free)
        g_callbacks.guest_free(g_context, locked.guest_bits);
    g_texture_locks.erase(it);
    return status;
}

HRESULT lock_cube_texture_rect(uint32_t guest_texture,
                               IDirect3DCubeTexture9 *texture,
                               const uint32_t *args)
{
    if (args[1] > D3DCUBEMAP_FACE_NEGATIVE_Z) {
        set_error("IDirect3DCubeTexture9::LockRect face is outside the six cube faces");
        return D3DERR_INVALIDCALL;
    }
    if (!args[3]) {
        set_error("IDirect3DCubeTexture9::LockRect output pointer is null");
        return D3DERR_INVALIDCALL;
    }
    auto *guest_output = static_cast<GuestLockedRect *>(guest_memory(
        args[3], sizeof(GuestLockedRect), AC1_D3D9_GUEST_WRITE));
    if (!guest_output) {
        set_error("IDirect3DCubeTexture9::LockRect output structure is unmapped");
        return D3DERR_INVALIDCALL;
    }

    const TextureLockKey key = texture_lock_key(guest_texture, args[1], args[2]);
    if (g_texture_locks.find(key) != g_texture_locks.end()) {
        set_error("IDirect3DCubeTexture9::LockRect face/level is already locked");
        return D3DERR_INVALIDCALL;
    }

    D3DSURFACE_DESC description{};
    HRESULT status = texture->GetLevelDesc(args[2], &description);
    if (FAILED(status)) return status;
    if (description.Format != D3DFMT_A8R8G8B8) {
        set_error("cube LockRect supports only the observed A8R8G8B8 format");
        return D3DERR_NOTAVAILABLE;
    }
    const uint64_t row_bytes_64 = static_cast<uint64_t>(description.Width) * 4u;
    if (!description.Width || !description.Height ||
        description.Width != description.Height || row_bytes_64 > UINT32_MAX) {
        set_error("cube LockRect dimensions exceed the square guest staging ABI");
        return D3DERR_INVALIDCALL;
    }

    D3DLOCKED_RECT native{};
    status = texture->LockRect(static_cast<D3DCUBEMAP_FACES>(args[1]), args[2],
                               &native, nullptr, 0);
    if (FAILED(status)) return status;
    if (!native.pBits || native.Pitch <= 0 ||
        static_cast<uint32_t>(native.Pitch) < row_bytes_64) {
        texture->UnlockRect(static_cast<D3DCUBEMAP_FACES>(args[1]), args[2]);
        set_error("native cube LockRect returned an unsupported pitch or null bits");
        return D3DERR_INVALIDCALL;
    }

    const uint64_t size_64 = static_cast<uint64_t>(native.Pitch) * description.Height;
    if (size_64 > UINT32_MAX || !size_64) {
        texture->UnlockRect(static_cast<D3DCUBEMAP_FACES>(args[1]), args[2]);
        set_error("native cube LockRect span overflows 32-bit guest memory");
        return D3DERR_INVALIDCALL;
    }
    const uint32_t allocation_size = static_cast<uint32_t>(size_64);
    const uint32_t guest_bits = g_callbacks.guest_alloc
        ? g_callbacks.guest_alloc(g_context, allocation_size, 4) : 0;
    if (!guest_bits) {
        texture->UnlockRect(static_cast<D3DCUBEMAP_FACES>(args[1]), args[2]);
        set_error("guest allocation failed for the locked cube staging buffer");
        return E_OUTOFMEMORY;
    }
    auto *staging = static_cast<uint8_t *>(guest_memory(
        guest_bits, allocation_size, AC1_D3D9_GUEST_WRITE));
    if (!staging) {
        g_callbacks.guest_free(g_context, guest_bits);
        texture->UnlockRect(static_cast<D3DCUBEMAP_FACES>(args[1]), args[2]);
        set_error("allocated cube staging buffer is not writable in guest memory");
        return D3DERR_INVALIDCALL;
    }
    std::memset(staging, 0, allocation_size);
    const uint32_t pitch = static_cast<uint32_t>(native.Pitch);
    const uint32_t row_bytes = static_cast<uint32_t>(row_bytes_64);
    for (uint32_t row = 0; row < description.Height; ++row) {
        std::memcpy(staging + static_cast<size_t>(row) * pitch,
                    static_cast<const uint8_t *>(native.pBits) +
                        static_cast<size_t>(row) * pitch,
                    row_bytes);
    }

    try {
        g_texture_locks.emplace(key, TextureLock{
            native.pBits, guest_bits, allocation_size, pitch, row_bytes,
            description.Height, args[1], args[2]});
    } catch (...) {
        g_callbacks.guest_free(g_context, guest_bits);
        texture->UnlockRect(static_cast<D3DCUBEMAP_FACES>(args[1]), args[2]);
        set_error("failed to register the guest cube lock");
        return E_OUTOFMEMORY;
    }

    const GuestLockedRect locked{native.Pitch, guest_bits};
    std::memcpy(guest_output, &locked, sizeof(locked));
    return status;
}

HRESULT unlock_cube_texture_rect(uint32_t guest_texture,
                                IDirect3DCubeTexture9 *texture,
                                uint32_t face, uint32_t level)
{
    if (face > D3DCUBEMAP_FACE_NEGATIVE_Z) {
        set_error("IDirect3DCubeTexture9::UnlockRect face is outside the six cube faces");
        return D3DERR_INVALIDCALL;
    }
    const TextureLockKey key = texture_lock_key(guest_texture, face, level);
    auto it = g_texture_locks.find(key);
    if (it == g_texture_locks.end()) {
        set_error("IDirect3DCubeTexture9::UnlockRect has no matching face/level lock");
        return D3DERR_INVALIDCALL;
    }
    TextureLock &locked = it->second;
    auto *staging = static_cast<const uint8_t *>(guest_memory(
        locked.guest_bits, locked.allocation_size, AC1_D3D9_GUEST_READ));
    if (!staging) {
        set_error("locked cube staging buffer became unreadable in guest memory");
        return D3DERR_INVALIDCALL;
    }
    for (uint32_t row = 0; row < locked.rows; ++row) {
        std::memcpy(static_cast<uint8_t *>(locked.native_bits) +
                        static_cast<size_t>(row) * locked.pitch,
                    staging + static_cast<size_t>(row) * locked.pitch,
                    locked.row_bytes);
    }
    const HRESULT status = texture->UnlockRect(
        static_cast<D3DCUBEMAP_FACES>(face), level);
    if (FAILED(status)) return status;
    if (g_callbacks.guest_free)
        g_callbacks.guest_free(g_context, locked.guest_bits);
    g_texture_locks.erase(it);
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

bool read_shader_model_3(uint32_t guest_bytecode, DWORD expected_version,
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
    /* The observed asset is pixel shader 3.0. Reject other profiles until a
     * parser for their different instruction-length rules is evidenced. */
    if (token != expected_version) {
        std::ostringstream message;
        message << "Create" << shader_kind
                << " only supports the observed shader model 3.0 token format";
        set_error(message.str());
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
        } else {
            payload_words = (token & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT;
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
    if (!read_shader_model_3(args[1], expected_version, shader_kind, &bytecode))
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

#if defined(_WIN32)
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
        const char *parent = native_constant_handle(args[0], args[1], true);
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
        const uint32_t native_handle = native_constant_handle(args[0], args[1], false)
            ? args[1] : 0;
        if (!native_handle) return D3DERR_INVALIDCALL;
        const char *handle = native_constant_handle(args[0], args[1], false);
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
    return D3DERR_NOTAVAILABLE;
}
#endif

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
#if defined(_WIN32)
    return hresult(invoke_constant_table_method(object, method, args, argc));
#else
    (void)method;
    (void)args;
    (void)argc;
    set_error("ID3DXConstantTable native methods are unsupported on this platform");
    return hresult(D3DERR_NOTAVAILABLE);
#endif
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
    if (!read_shader_model_3(guest_bytecode, version,
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
    if (method == 38) {
        if (!require_count(argc, 3, "IDirect3DDevice9::GetRenderTarget"))
            return hresult(D3DERR_INVALIDCALL);
        return hresult(get_device_surface(static_cast<IDirect3DDevice9 *>(object.native),
                                          args, false));
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
        /* Non-null texture handles are rejected as unsupported before dispatch. */
        return hresult(device->SetTexture(args[1], nullptr));
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
    std::ostringstream message;
    message << "IDirect3DDevice9 method " << method
            << " is not implemented; no guest device/frame success is implied";
    set_error(message.str());
    return hresult(D3DERR_NOTAVAILABLE);
}

uint32_t invoke_pixel_shader(GuestObject &object, uint32_t method,
                             const uint32_t *args, uint32_t argc)
{
    const std::string prefix = interface_name(object.kind);
    if (method == 0) {
        if (!require_count(argc, 3, (prefix + "::QueryInterface").c_str()))
            return hresult(D3DERR_INVALIDCALL);
        uint32_t output = 0;
        return hresult(do_query_interface(object, args, &output));
    }
    if (method == 1) {
        if (!require_count(argc, 1, (prefix + "::AddRef").c_str())) return 0;
        ++object.guest_references;
        object.native->AddRef();
        return object.guest_references;
    }
    if (method == 2) {
        if (!require_count(argc, 1, (prefix + "::Release").c_str())) return 0;
        if (!object.guest_references) {
            set_error(prefix + "::Release called with zero guest references");
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
    set_error(prefix + " method is not implemented");
    return hresult(D3DERR_NOTAVAILABLE);
}

uint32_t invoke_surface(GuestObject &object, uint32_t method,
                        const uint32_t *args, uint32_t argc)
{
    if (method == 0) {
        if (!require_count(argc, 3, "IDirect3DSurface9::QueryInterface"))
            return hresult(D3DERR_INVALIDCALL);
        uint32_t output = 0;
        return hresult(do_query_interface(object, args, &output));
    }
    if (method == 3) {
        if (!require_count(argc, 2, "IDirect3DSurface9::GetDevice"))
            return hresult(D3DERR_INVALIDCALL);
        auto *output = static_cast<uint32_t *>(guest_memory(
            args[1], sizeof(uint32_t), AC1_D3D9_GUEST_WRITE));
        if (!output) {
            set_error("IDirect3DSurface9::GetDevice output pointer is unmapped");
            return hresult(D3DERR_INVALIDCALL);
        }
        *output = 0;
        IDirect3DDevice9 *native_device = nullptr;
        const HRESULT status = static_cast<IDirect3DSurface9 *>(object.native)->GetDevice(
            &native_device);
        if (FAILED(status)) return hresult(status);
        if (!native_device) {
            set_error("IDirect3DSurface9::GetDevice succeeded without returning a device");
            return hresult(E_FAIL);
        }
        const uint32_t guest_device = wrap_native(
            static_cast<IUnknown *>(native_device), ObjectKind::Device, true);
        if (!guest_device) return hresult(E_OUTOFMEMORY);
        *output = guest_device;
        return hresult(status);
    }
    if (method == 1) {
        if (!require_count(argc, 1, "IDirect3DSurface9::AddRef")) return 0;
        ++object.guest_references;
        object.native->AddRef();
        return object.guest_references;
    }
    if (method == 2) {
        if (!require_count(argc, 1, "IDirect3DSurface9::Release")) return 0;
        if (!object.guest_references) {
            set_error("IDirect3DSurface9::Release called with zero guest references");
            return 0;
        }
        IUnknown *native = object.native;
        const uint32_t guest_address = args[0];
        native->Release();
        const uint32_t guest_count = --object.guest_references;
        if (guest_count == 0) {
            GuestObject retired = object;
            g_guest_by_native.erase(native);
            g_objects.erase(guest_address);
            free_guest_object(guest_address, retired);
        }
        return guest_count;
    }
    if (method == 12) {
        if (!require_count(argc, 2, "IDirect3DSurface9::GetDesc"))
            return hresult(D3DERR_INVALIDCALL);
        auto *output = static_cast<D3DSURFACE_DESC *>(guest_memory(
            args[1], sizeof(D3DSURFACE_DESC), AC1_D3D9_GUEST_WRITE));
        if (!output) {
            set_error("IDirect3DSurface9::GetDesc output is unmapped");
            return hresult(D3DERR_INVALIDCALL);
        }
        D3DSURFACE_DESC description{};
        const HRESULT status = static_cast<IDirect3DSurface9 *>(object.native)->GetDesc(
            &description);
        if (SUCCEEDED(status)) std::memcpy(output, &description, sizeof(description));
        return hresult(status);
    }

    std::ostringstream message;
    message << "IDirect3DSurface9 method " << method
            << " is not implemented; no surface success is implied";
    set_error(message.str());
    return hresult(D3DERR_NOTAVAILABLE);
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
    set_error(object.kind == ObjectKind::VertexBuffer
        ? "IDirect3DVertexBuffer9 method is not implemented"
        : "IDirect3DIndexBuffer9 method is not implemented");
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

uint32_t invoke_texture(GuestObject &object, uint32_t method,
                        const uint32_t *args, uint32_t argc)
{
    const char *name = interface_name(object.kind);
    if (method == 0) {
        if (!require_count(argc, 3, "IDirect3D texture::QueryInterface"))
            return hresult(D3DERR_INVALIDCALL);
        uint32_t output = 0;
        return hresult(do_query_interface(object, args, &output));
    }
    if (method == 1) {
        if (!require_count(argc, 1, "IDirect3D texture::AddRef")) return 0;
        ++object.guest_references;
        object.native->AddRef();
        return object.guest_references;
    }
    if (method == 2) {
        if (!require_count(argc, 1, "IDirect3D texture::Release")) return 0;
        if (!object.guest_references) {
            set_error(std::string(name) + "::Release called with zero guest references");
            return 0;
        }
        IUnknown *native = object.native;
        const uint32_t guest_address = args[0];
        const uint32_t guest_count = object.guest_references - 1;
        if (!guest_count && object.kind == ObjectKind::Texture)
            discard_texture_locks(guest_address,
                static_cast<IDirect3DTexture9 *>(object.native));
        if (!guest_count && object.kind == ObjectKind::CubeTexture)
            discard_cube_texture_locks(guest_address,
                static_cast<IDirect3DCubeTexture9 *>(object.native));
        native->Release();
        object.guest_references = guest_count;
        if (!guest_count) {
            GuestObject retired = object;
            g_guest_by_native.erase(native);
            g_objects.erase(guest_address);
            free_guest_object(guest_address, retired);
        }
        return guest_count;
    }
    if (object.kind == ObjectKind::CubeTexture) {
        auto *cube = static_cast<IDirect3DCubeTexture9 *>(object.native);
        if (method == 19) {
            if (!require_count(argc, 6, "IDirect3DCubeTexture9::LockRect"))
                return hresult(D3DERR_INVALIDCALL);
            return hresult(lock_cube_texture_rect(args[0], cube, args));
        }
        if (method == 20) {
            if (!require_count(argc, 3, "IDirect3DCubeTexture9::UnlockRect"))
                return hresult(D3DERR_INVALIDCALL);
            return hresult(unlock_cube_texture_rect(args[0], cube,
                                                     args[1], args[2]));
        }
        set_error("IDirect3DCubeTexture9 method is not implemented");
        return hresult(D3DERR_NOTAVAILABLE);
    }
    if (method == 19) {
        if (!require_count(argc, 5, "IDirect3DTexture9::LockRect"))
            return hresult(D3DERR_INVALIDCALL);
        return hresult(lock_texture_rect(args[0],
            static_cast<IDirect3DTexture9 *>(object.native), args));
    }
    if (method == 20) {
        if (!require_count(argc, 2, "IDirect3DTexture9::UnlockRect"))
            return hresult(D3DERR_INVALIDCALL);
        return hresult(unlock_texture_rect(args[0],
            static_cast<IDirect3DTexture9 *>(object.native), args[1]));
    }
    set_error("IDirect3DTexture9 method is not implemented");
    return hresult(D3DERR_NOTAVAILABLE);
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
#if defined(_WIN32)
    GuestObject *device_object = find_object(guest_device, ObjectKind::Device);
    if (!device_object) {
        set_error("D3DXCheckTextureRequirements received an unknown guest D3D9 device");
        return -1;
    }
    if (!ensure_d3dx9_module()) return -2;

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

    const HRESULT status = g_d3dx_check_texture_requirements(
        static_cast<IDirect3DDevice9 *>(device_object->native),
        reinterpret_cast<UINT *>(native_values[0]),
        reinterpret_cast<UINT *>(native_values[1]),
        reinterpret_cast<UINT *>(native_values[2]), usage,
        reinterpret_cast<D3DFORMAT *>(native_values[3]), static_cast<D3DPOOL>(pool));
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
#else
    (void)guest_device;
    (void)guest_width;
    (void)guest_height;
    (void)guest_mip_levels;
    (void)usage;
    (void)guest_format;
    (void)pool;
    set_error("D3DXCheckTextureRequirements is not supported by this platform bridge");
    return -2;
#endif
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

#if defined(_WIN32)
    std::vector<DWORD> bytecode;
    const int scan_status = scan_shader_for_d3dx(guest_bytecode, &bytecode);
    if (scan_status != 1) return scan_status;
    if (!ensure_d3dx_shader_table_export()) return -2;

    ID3DXConstantTable *native_table = nullptr;
    const HRESULT status = g_d3dx_get_shader_constant_table(
        bytecode.data(), &native_table);
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
#else
    (void)guest_bytecode;
    set_error("D3DXGetShaderConstantTable is unsupported on this platform");
    return -2;
#endif
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
    if (object->kind == ObjectKind::Device && method == 65 && args[2] != 0u) {
        set_error("non-null texture guest object has no registered AC1 proxy kind");
        return -2;
    }
    if (object->kind == ObjectKind::Device && method == 25 && args[7] != 0u) {
        set_error("IDirect3DDevice9::CreateCubeTexture shared handles are unsupported");
        return -2;
    }
    if (object->kind == ObjectKind::CubeTexture && method == 19 &&
        (args[4] != 0u || args[5] != 0u)) {
        set_error("cube LockRect only supports the observed null RECT and zero flags");
        return -2;
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
    if (object->kind == ObjectKind::Device && (method == 91 || method == 106)) {
        uint32_t version = 0;
        if (guest_read(args[1], &version, sizeof(version))) {
            const uint32_t expected = method == 106 ? D3DPS_VERSION(3, 0)
                                                   : D3DVS_VERSION(3, 0);
            if (version != expected) {
                set_error(method == 106
                    ? "CreatePixelShader only supports the observed pixel shader 3.0 profile"
                    : "CreateVertexShader only supports the observed vertex shader 3.0 profile");
                return -2;
            }
        }
    }
    if (object->kind == ObjectKind::Texture && method == 19) {
        if (args[3] != 0u || args[4] != 0u) {
            set_error("texture LockRect only supports the observed null RECT and zero flags");
            return -2;
        }
        D3DSURFACE_DESC description{};
        const HRESULT desc_status = static_cast<IDirect3DTexture9 *>(object->native)->GetLevelDesc(
            args[1], &description);
        if (SUCCEEDED(desc_status) && description.Format != D3DFMT_A8R8G8B8) {
            set_error("texture LockRect format is outside the observed A8R8G8B8 profile");
            return -2;
        }
    }
    if (method == 0) {
        GUID iid{};
        if (guest_read(args[1], &iid, sizeof(iid)) &&
            !bridge_supports_iid(object->kind, iid)) {
            report_unsupported_iid(object->kind, iid);
            return -2;
        }
    }
    switch (object->kind) {
    case ObjectKind::D3D9:
        *result_out = invoke_d3d9(*object, method, args, arg_count);
        break;
    case ObjectKind::Device:
        *result_out = invoke_device(*object, method, args, arg_count);
        break;
    case ObjectKind::Surface:
        *result_out = invoke_surface(*object, method, args, arg_count);
        break;
    case ObjectKind::VertexBuffer:
    case ObjectKind::IndexBuffer:
        *result_out = invoke_buffer(*object, method, args, arg_count);
        break;
    case ObjectKind::VertexDeclaration:
        *result_out = invoke_vertex_declaration(*object, method, args, arg_count);
        break;
    case ObjectKind::Texture:
        *result_out = invoke_texture(*object, method, args, arg_count);
        break;
    case ObjectKind::CubeTexture:
        *result_out = invoke_texture(*object, method, args, arg_count);
        break;
    case ObjectKind::PixelShader:
        *result_out = invoke_pixel_shader(*object, method, args, arg_count);
        break;
    case ObjectKind::VertexShader:
        *result_out = invoke_pixel_shader(*object, method, args, arg_count);
        break;
    case ObjectKind::ConstantTable:
        *result_out = invoke_constant_table(*object, method, args, arg_count);
        break;
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
        if (entry.second.kind == ObjectKind::Texture)
            discard_texture_locks(entry.first,
                static_cast<IDirect3DTexture9 *>(entry.second.native));
        if (entry.second.kind == ObjectKind::CubeTexture)
            discard_cube_texture_locks(entry.first,
                static_cast<IDirect3DCubeTexture9 *>(entry.second.native));
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
    g_guest_by_native.clear();
    g_constant_handles.clear();
    g_guest_handle_by_native.clear();
    g_constant_table_allocations.clear();
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
