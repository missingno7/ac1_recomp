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
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

namespace {

constexpr uint32_t kTokenBase = 0x7e000000u;
constexpr uint32_t kKindStride = 0x1000u;
constexpr uint32_t kSlotStride = 0x10u;
constexpr uint32_t kD3D9Kind = 1u;
constexpr uint32_t kDeviceKind = 2u;
constexpr uint32_t kSurfaceKind = 3u;
constexpr uint32_t kD3D9VtableSlots = 17u;
constexpr uint32_t kDeviceVtableSlots = 119u;
constexpr uint32_t kSurfaceVtableSlots = 17u;

constexpr GUID kIidIUnknown =
    {0x00000000u, 0x0000u, 0x0000u, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIidIDirect3D9 =
    {0x81bdcbcau, 0x64d4u, 0x426du, {0xae, 0x8d, 0xad, 0x01, 0x47, 0xf4, 0x27, 0x5c}};
constexpr GUID kIidIDirect3DDevice9 =
    {0xd0223b96u, 0xbf7au, 0x43fdu, {0x92, 0xbd, 0xa4, 0x3b, 0x0d, 0x82, 0xb9, 0xeb}};
constexpr GUID kIidIDirect3DSurface9 =
    {0x0cfbaf3au, 0x9ff6u, 0x429au, {0x99, 0xb3, 0xa2, 0x79, 0x6a, 0xf8, 0xb8, 0x9b}};

enum class ObjectKind : uint32_t {
    D3D9 = kD3D9Kind,
    Device = kDeviceKind,
    Surface = kSurfaceKind
};

struct GuestObject {
    IUnknown *native = nullptr;
    ObjectKind kind = ObjectKind::D3D9;
    uint32_t guest_vtable = 0;
    uint32_t guest_references = 1;
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

ac1_d3d9_callbacks g_callbacks{};
void *g_context = nullptr;
std::map<uint32_t, GuestObject> g_objects;
std::map<IUnknown *, uint32_t> g_guest_by_native;
std::mutex g_mutex;
/* MinGW thread-exit crashes were reproduced with nontrivial TLS destructors. */
thread_local char g_last_error[1024]{};
#if defined(_WIN32)
HMODULE g_d3d9_module = nullptr;
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
    }
    return 0;
}

int token_argument_count(uint32_t token)
{
    if (token < kTokenBase || token >= kTokenBase + 4 * kKindStride) return 0;
    const uint32_t offset = token - kTokenBase;
    const uint32_t kind = offset / kKindStride;
    const uint32_t within_kind = offset % kKindStride;
    if (kind != kD3D9Kind && kind != kDeviceKind && kind != kSurfaceKind) return 0;
    if (within_kind % kSlotStride) return -1;
    const uint32_t method = within_kind / kSlotStride;
    if (method >= slot_count(static_cast<ObjectKind>(kind))) return -1;

    if (kind == kDeviceKind) {
        if (method < 3) return method == 0 ? 3 : 1;
        if (method == 38) return 3;
        if (method == 40) return 2;
        return -2;
    }
    if (kind == kSurfaceKind) {
        if (method < 3) return method == 0 ? 3 : 1;
        return method == 12 ? 2 : -2;
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

bool bridge_supports_iid(ObjectKind kind, const GUID &iid)
{
    const GUID &supported = kind == ObjectKind::D3D9
        ? kIidIDirect3D9
        : kind == ObjectKind::Device ? kIidIDirect3DDevice9
                                     : kIidIDirect3DSurface9;
    return std::memcmp(&iid, &kIidIUnknown, sizeof(iid)) == 0 ||
           std::memcmp(&iid, &supported, sizeof(iid)) == 0;
}

const char *interface_name(ObjectKind kind)
{
    switch (kind) {
    case ObjectKind::D3D9: return "IDirect3D9";
    case ObjectKind::Device: return "IDirect3DDevice9";
    case ObjectKind::Surface: return "IDirect3DSurface9";
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
    HRESULT status = d3d->CreateDevice(args[1], static_cast<D3DDEVTYPE>(args[2]),
                                       reinterpret_cast<HWND>(focus), args[4],
                                       &host, &device);
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
    std::ostringstream message;
    message << "IDirect3DDevice9 method " << method
            << " is not implemented; no guest device/frame success is implied";
    set_error(message.str());
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
        if (!g_d3d9_module) {
            wchar_t system_directory[MAX_PATH]{};
            UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
            if (!length || length >= MAX_PATH - 10) {
                set_error("GetSystemDirectoryW failed while loading native D3D9");
                return 0;
            }
            std::wstring path(system_directory, length);
            path += L"\\d3d9.dll";
            g_d3d9_module = LoadLibraryW(path.c_str());
            if (!g_d3d9_module) {
                set_error("failed to load the system d3d9.dll");
                return 0;
            }
        }
        using Factory = IDirect3D9 *(WINAPI *)(UINT);
        FARPROC raw_factory = GetProcAddress(g_d3d9_module, "Direct3DCreate9");
        static_assert(sizeof(Factory) == sizeof(raw_factory),
                      "Windows function-pointer ABI mismatch");
        Factory factory = nullptr;
        std::memcpy(&factory, &raw_factory, sizeof(factory));
        if (!factory) {
            set_error("system d3d9.dll has no Direct3DCreate9 export");
            return 0;
        }
        native = factory(sdk_version);
#else
        set_error("no platform D3D9 factory was supplied");
        return 0;
#endif
    }
    if (!native) {
        set_error("Direct3DCreate9 returned null");
        return 0;
    }
    uint32_t address = wrap_native(native, ObjectKind::D3D9, true);
    if (!address) set_error("failed to wrap Direct3DCreate9 result");
    return address;
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
        GuestObject &object = entry.second;
        for (uint32_t refs = object.guest_references; refs; --refs)
            object.native->Release();
        free_guest_object(entry.first, object);
    }
    g_objects.clear();
    g_guest_by_native.clear();
#if defined(_WIN32)
    if (g_d3d9_module) {
        FreeLibrary(g_d3d9_module);
        g_d3d9_module = nullptr;
    }
#endif
}

extern "C" AC1_D3D9_API const char *ac1_d3d9_last_error(void)
{
    return g_last_error;
}
