#include "ac1_d3d9_bridge.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <windows.h>
#include <d3d9.h>

namespace {
constexpr uint32_t kGuestBase = 0x00010000u;
constexpr uint32_t kGuestLimit = 0x00050000u;
constexpr uint32_t kGuestObjectPool = 0x00020000u;
std::array<uint8_t, kGuestLimit - kGuestBase> guest_memory{};
uint32_t next_guest_address = kGuestObjectPool;
HWND hidden_window = nullptr;
std::array<void *, 17> fake_d3d_vtable{};
std::array<void *, 119> fake_device_vtable{};
std::array<void *, 17> fake_surface_vtable{};

void *resolve_guest(void *context, uint32_t address, uint32_t size, uint32_t access);
uint32_t allocate_guest(void *context, uint32_t size, uint32_t alignment);
uint32_t guest_word(uint32_t address);
uint32_t object_slot(uint32_t object, uint32_t slot);

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
static_assert(sizeof(GuestPresentParameters) == 56, "guest D3D9 ABI test layout");

struct FakeComObject {
    void **vtable;
    ULONG references;
    D3DSURFACE_DESC description;
};
FakeComObject fake_d3d{};
FakeComObject fake_device{};
FakeComObject fake_render_target{};
FakeComObject fake_depth_surface{};
uint32_t fake_query_calls = 0;

HRESULT STDMETHODCALLTYPE fake_query_interface(IUnknown *self, REFIID iid, void **output)
{
    static const GUID iid_unknown =
        {0x00000000u, 0x0000u, 0x0000u, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
    if (!output) return E_POINTER;
    *output = nullptr;
    ++fake_query_calls;
    if (std::memcmp(&iid, &iid_unknown, sizeof(iid_unknown)) != 0) return E_NOINTERFACE;
    ++reinterpret_cast<FakeComObject *>(self)->references;
    *output = self;
    return S_OK;
}

ULONG STDMETHODCALLTYPE fake_add_ref(IUnknown *self)
{
    return ++reinterpret_cast<FakeComObject *>(self)->references;
}

ULONG STDMETHODCALLTYPE fake_release(IUnknown *self)
{
    FakeComObject *object = reinterpret_cast<FakeComObject *>(self);
    return object->references ? --object->references : 0;
}

HRESULT STDMETHODCALLTYPE fake_create_device(
    IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **output)
{
    if (!output) return E_POINTER;
    fake_device.references = 1;
    *output = reinterpret_cast<IDirect3DDevice9 *>(&fake_device);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_get_render_target(
    IDirect3DDevice9 *, DWORD index, IDirect3DSurface9 **output)
{
    if (!output) return E_POINTER;
    *output = nullptr;
    if (index != 0) return D3DERR_INVALIDCALL;
    ++fake_render_target.references;
    *output = reinterpret_cast<IDirect3DSurface9 *>(&fake_render_target);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_get_depth_surface(
    IDirect3DDevice9 *, IDirect3DSurface9 **output)
{
    if (!output) return E_POINTER;
    ++fake_depth_surface.references;
    *output = reinterpret_cast<IDirect3DSurface9 *>(&fake_depth_surface);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE fake_get_surface_desc(IDirect3DSurface9 *self,
                                                  D3DSURFACE_DESC *output)
{
    if (!output) return D3DERR_INVALIDCALL;
    *output = reinterpret_cast<FakeComObject *>(self)->description;
    return S_OK;
}

void *fake_factory(void *, uint32_t)
{
    return reinterpret_cast<IDirect3D9 *>(&fake_d3d);
}

void initialize_fake_com()
{
    fake_d3d_vtable.fill(nullptr);
    fake_device_vtable.fill(nullptr);
    fake_surface_vtable.fill(nullptr);
    for (auto *table : {&fake_d3d_vtable, &fake_surface_vtable}) {
        (*table)[0] = reinterpret_cast<void *>(&fake_query_interface);
        (*table)[1] = reinterpret_cast<void *>(&fake_add_ref);
        (*table)[2] = reinterpret_cast<void *>(&fake_release);
    }
    fake_device_vtable[0] = reinterpret_cast<void *>(&fake_query_interface);
    fake_device_vtable[1] = reinterpret_cast<void *>(&fake_add_ref);
    fake_device_vtable[2] = reinterpret_cast<void *>(&fake_release);
    fake_d3d_vtable[16] = reinterpret_cast<void *>(&fake_create_device);
    fake_device_vtable[38] = reinterpret_cast<void *>(&fake_get_render_target);
    fake_device_vtable[40] = reinterpret_cast<void *>(&fake_get_depth_surface);
    fake_surface_vtable[12] = reinterpret_cast<void *>(&fake_get_surface_desc);

    fake_d3d = {fake_d3d_vtable.data(), 1, {}};
    fake_device = {fake_device_vtable.data(), 0, {}};
    fake_query_calls = 0;
    fake_render_target = {fake_surface_vtable.data(), 0,
        {D3DFMT_A8R8G8B8, D3DRTYPE_SURFACE, D3DUSAGE_RENDERTARGET,
         D3DPOOL_DEFAULT, D3DMULTISAMPLE_NONE, 0, 640, 480}};
    fake_depth_surface = {fake_surface_vtable.data(), 0,
        {D3DFMT_D24S8, D3DRTYPE_SURFACE, D3DUSAGE_DEPTHSTENCIL,
         D3DPOOL_DEFAULT, D3DMULTISAMPLE_NONE, 0, 640, 480}};
}

bool run_fake_surface_smoke(ac1_d3d9_callbacks callbacks)
{
    initialize_fake_com();
    callbacks.factory = fake_factory;
    if (!ac1_d3d9_configure(&callbacks, nullptr)) {
        std::fprintf(stderr, "fake bridge configure failed: %s\n", ac1_d3d9_last_error());
        return false;
    }
    const uint32_t d3d = ac1_d3d9_create9(0x20);
    if (!d3d) return false;

    const uint32_t pp_address = allocate_guest(nullptr, sizeof(GuestPresentParameters), 4);
    const uint32_t device_output = allocate_guest(nullptr, 4, 4);
    GuestPresentParameters pp{};
    pp.back_buffer_width = 640;
    pp.back_buffer_height = 480;
    pp.back_buffer_count = 1;
    pp.swap_effect = 1;
    pp.device_window = 0x1234u;
    pp.windowed = 1;
    pp.enable_auto_depth_stencil = 1;
    pp.auto_depth_stencil_format = D3DFMT_D24S8;
    std::memcpy(resolve_guest(nullptr, pp_address, sizeof(pp), AC1_D3D9_GUEST_WRITE), &pp,
                sizeof(pp));
    const uint32_t create_args[] = {d3d, 0, D3DDEVTYPE_HAL, 0x1234u,
                                    D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                    pp_address, device_output};
    uint32_t result = 0;
    if (ac1_d3d9_dispatch(0x7e001000u + 16 * 0x10u, create_args, 7, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result))) {
        std::fprintf(stderr, "fake CreateDevice failed: 0x%08x %s\n", result,
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t device = guest_word(device_output);
    const uint32_t render_output = allocate_guest(nullptr, 4, 4);
    const uint32_t depth_output = allocate_guest(nullptr, 4, 4);
    uint32_t bad_output = 0xdeadbeefu;
    std::memcpy(resolve_guest(nullptr, render_output, sizeof(bad_output), AC1_D3D9_GUEST_WRITE),
                &bad_output, sizeof(bad_output));
    const uint32_t bad_render_args[] = {device, 1, render_output};
    if (ac1_d3d9_dispatch(object_slot(device, 38), bad_render_args, 3, &result) != 1 ||
        result != static_cast<uint32_t>(D3DERR_INVALIDCALL) || guest_word(render_output) != 0) {
        std::fprintf(stderr, "fake GetRenderTarget HRESULT/output preservation failed\n");
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t render_args[] = {device, 0, render_output};
    const uint32_t depth_args[] = {device, depth_output};
    if (ac1_d3d9_dispatch(object_slot(device, 38), render_args, 3, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result)) ||
        ac1_d3d9_dispatch(object_slot(device, 40), depth_args, 2, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result))) {
        std::fprintf(stderr, "fake surface getter failed: 0x%08x %s\n", result,
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }

    const uint32_t surface = guest_word(render_output);
    const uint32_t desc_address = allocate_guest(nullptr, sizeof(D3DSURFACE_DESC), 4);
    const uint32_t desc_args[] = {surface, desc_address};
    if (ac1_d3d9_dispatch(object_slot(surface, 12), desc_args, 2, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result)) ||
        fake_render_target.references != 1) {
        std::fprintf(stderr, "fake surface GetDesc/ref failed: 0x%08x %s\n", result,
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    uint32_t description_type = 0, description_width = 0, description_height = 0;
    std::memcpy(&description_type,
                resolve_guest(nullptr, desc_address + 4, sizeof(uint32_t), AC1_D3D9_GUEST_READ),
                sizeof(uint32_t));
    std::memcpy(&description_width,
                resolve_guest(nullptr, desc_address + 24, sizeof(uint32_t), AC1_D3D9_GUEST_READ),
                sizeof(uint32_t));
    std::memcpy(&description_height,
                resolve_guest(nullptr, desc_address + 28, sizeof(uint32_t), AC1_D3D9_GUEST_READ),
                sizeof(uint32_t));
    if (description_type != D3DRTYPE_SURFACE || description_width != 640 ||
        description_height != 480) {
        std::fprintf(stderr, "fake surface GetDesc marshaling mismatch (%u, %u x %u)\n",
                     description_type, description_width, description_height);
        ac1_d3d9_shutdown();
        return false;
    }

    const uint8_t iid_unimplemented[16] = {
        0x44, 0x33, 0x22, 0x11, 0x66, 0x55, 0x88, 0x77,
        0, 1, 2, 3, 4, 5, 6, 7
    };
    const uint32_t unsupported_iid_address = allocate_guest(
        nullptr, sizeof(iid_unimplemented), 4);
    const uint32_t unsupported_output = allocate_guest(nullptr, 4, 4);
    const uint32_t output_sentinel = 0xc001d00du;
    std::memcpy(resolve_guest(nullptr, unsupported_iid_address, sizeof(iid_unimplemented),
                              AC1_D3D9_GUEST_WRITE),
                iid_unimplemented, sizeof(iid_unimplemented));
    std::memcpy(resolve_guest(nullptr, unsupported_output, sizeof(output_sentinel),
                              AC1_D3D9_GUEST_WRITE),
                &output_sentinel, sizeof(output_sentinel));
    const uint32_t unsupported_args[] = {
        surface, unsupported_iid_address, unsupported_output
    };
    const uint32_t query_calls_before = fake_query_calls;
    result = 0xdeadbeefu;
    if (ac1_d3d9_dispatch(object_slot(surface, 0), unsupported_args, 3, &result) != -2 ||
        result != 0xdeadbeefu || guest_word(unsupported_output) != output_sentinel ||
        fake_query_calls != query_calls_before ||
        std::strstr(ac1_d3d9_last_error(), "11223344-5566-7788") == nullptr ||
        std::strstr(ac1_d3d9_last_error(), "native QueryInterface was not called") == nullptr) {
        std::fprintf(stderr, "unsupported IID was mistaken for a native E_NOINTERFACE: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }

    const uint8_t iid_unknown[16] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46
    };
    const uint32_t iid_address = allocate_guest(nullptr, sizeof(iid_unknown), 4);
    const uint32_t queried_output = allocate_guest(nullptr, 4, 4);
    std::memcpy(resolve_guest(nullptr, iid_address, sizeof(iid_unknown), AC1_D3D9_GUEST_WRITE),
                iid_unknown, sizeof(iid_unknown));
    const uint32_t query_args[] = {surface, iid_address, queried_output};
    if (ac1_d3d9_dispatch(object_slot(surface, 0), query_args, 3, &result) != 1 ||
        FAILED(static_cast<HRESULT>(result)) || guest_word(queried_output) != surface ||
        fake_render_target.references != 2) {
        std::fprintf(stderr, "fake IUnknown QueryInterface failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t surface_this[] = {surface};
    uint32_t references = 0;
    if (ac1_d3d9_dispatch(object_slot(surface, 1), surface_this, 1, &references) != 1 ||
        references != 3 || fake_render_target.references != 3 ||
        ac1_d3d9_dispatch(object_slot(surface, 2), surface_this, 1, &references) != 1 ||
        references != 2 || fake_render_target.references != 2 ||
        ac1_d3d9_dispatch(object_slot(surface, 2), surface_this, 1, &references) != 1 ||
        references != 1 || fake_render_target.references != 1 ||
        ac1_d3d9_dispatch(object_slot(surface, 2), surface_this, 1, &references) != 1 ||
        references != 0 || fake_render_target.references != 0) {
        std::fprintf(stderr, "fake surface native lifetime balance failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }

    const uint32_t depth = guest_word(depth_output);
    const uint32_t depth_this[] = {depth};
    if (ac1_d3d9_dispatch(object_slot(depth, 2), depth_this, 1, &references) != 1 ||
        references != 0 || fake_depth_surface.references != 0) {
        std::fprintf(stderr, "fake depth surface release failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    const uint32_t device_this[] = {device};
    const uint32_t d3d_this[] = {d3d};
    if (ac1_d3d9_dispatch(object_slot(device, 2), device_this, 1, &references) != 1 ||
        references != 0 || fake_device.references != 0 ||
        ac1_d3d9_dispatch(object_slot(d3d, 2), d3d_this, 1, &references) != 1 ||
        references != 0 || fake_d3d.references != 0) {
        std::fprintf(stderr, "fake device/factory release failed: %s\n",
                     ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return false;
    }
    ac1_d3d9_shutdown();
    std::printf("fake D3D9 surface proxy smoke passed (GetRenderTarget/GetDepthStencilSurface/GetDesc and native refcounts)\n");
    return true;
}

void *resolve_guest(void *, uint32_t address, uint32_t size, uint32_t)
{
    if (address < kGuestBase || address > kGuestLimit ||
        size > kGuestLimit - address) return nullptr;
    return guest_memory.data() + (address - kGuestBase);
}

uint32_t allocate_guest(void *, uint32_t size, uint32_t alignment)
{
    const uint32_t mask = alignment - 1;
    next_guest_address = (next_guest_address + mask) & ~mask;
    if (!size || next_guest_address > kGuestLimit || size > kGuestLimit - next_guest_address)
        return 0;
    uint32_t result = next_guest_address;
    next_guest_address += size;
    return result;
}

void free_guest(void *, uint32_t) {}

uintptr_t translate_window(void *, uint32_t guest_hwnd)
{
    return guest_hwnd == 0x1234u
        ? reinterpret_cast<uintptr_t>(hidden_window) : 0;
}

uint32_t guest_word(uint32_t address)
{
    uint32_t value = 0;
    std::memcpy(&value, resolve_guest(nullptr, address, sizeof(value), AC1_D3D9_GUEST_READ),
                sizeof(value));
    return value;
}

uint32_t object_slot(uint32_t object, uint32_t slot)
{
    const uint32_t table = guest_word(object);
    return guest_word(table + slot * sizeof(uint32_t));
}

void log_message(void *, const char *message)
{
    std::fprintf(stderr, "bridge: %s\n", message);
}
}

int main()
{
    ac1_d3d9_callbacks callbacks{};
    callbacks.struct_size = sizeof(callbacks);
    callbacks.guest_resolve = resolve_guest;
    callbacks.guest_alloc = allocate_guest;
    callbacks.guest_free = free_guest;
    callbacks.guest_hwnd_to_host = translate_window;
    callbacks.log = log_message;
    if (!ac1_d3d9_configure(&callbacks, nullptr)) {
        std::fprintf(stderr, "configure failed: %s\n", ac1_d3d9_last_error());
        return 1;
    }

    const uint32_t d3d9 = ac1_d3d9_create9(0x20);
    if (!d3d9) {
        std::fprintf(stderr, "Direct3DCreate9 failed: %s\n", ac1_d3d9_last_error());
        return 2;
    }
    uint32_t vtable = 0;
    std::memcpy(&vtable, resolve_guest(nullptr, d3d9, sizeof(vtable), AC1_D3D9_GUEST_READ),
                sizeof(vtable));
    uint32_t get_adapter_count = 0;
    std::memcpy(&get_adapter_count,
                resolve_guest(nullptr, vtable + 4 * 4, sizeof(get_adapter_count), AC1_D3D9_GUEST_READ),
                sizeof(get_adapter_count));
    if (ac1_d3d9_argument_count(get_adapter_count) != 1 ||
        ac1_d3d9_argument_count(0x7e002030u) != -2 ||
        ac1_d3d9_argument_count(0x7e002260u) != 3 ||
        ac1_d3d9_argument_count(0x7e002280u) != 2 ||
        ac1_d3d9_argument_count(0x7e003000u) != 3 ||
        ac1_d3d9_argument_count(0x7e0030c0u) != 2 ||
        ac1_d3d9_argument_count(0x7e0030b0u) != -2 ||
        ac1_d3d9_argument_count(0x7e004000u) != 0 ||
        ac1_d3d9_dispatch(0x7e002030u, nullptr, 0, nullptr) != -2 ||
        ac1_d3d9_dispatch(0x7e0030b0u, nullptr, 0, nullptr) != -2 ||
        ac1_d3d9_dispatch(0x7e004000u, nullptr, 0, nullptr) != 0) {
        std::fprintf(stderr, "COM method-arity classification failed\n");
        ac1_d3d9_shutdown();
        return 3;
    }

    const uint32_t this_args[] = {d3d9};
    uint32_t adapter_count = UINT32_MAX;
    if (ac1_d3d9_dispatch(get_adapter_count, this_args, 1, &adapter_count) != 1 ||
        adapter_count > 1024) {
        std::fprintf(stderr, "GetAdapterCount dispatch failed: %s\n", ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return 4;
    }

    const uint32_t addref_token = 0x7e001000u + 1 * 0x10u;
    uint32_t references = 0;
    if (ac1_d3d9_dispatch(addref_token, this_args, 1, &references) != 1 || references != 2) {
        std::fprintf(stderr, "AddRef dispatch failed: %s\n", ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return 5;
    }
    const uint32_t release_token = 0x7e001000u + 2 * 0x10u;
    uint32_t remaining = UINT32_MAX;
    if (ac1_d3d9_dispatch(release_token, this_args, 1, &remaining) != 1 || remaining != 1) {
        std::fprintf(stderr, "Release dispatch failed: %s\n", ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return 6;
    }
    /* A hidden window plus windowed presentation avoids changing desktop mode. */
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"AC1D3D9BridgeSmokeWindow";
    if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        std::fprintf(stderr, "hidden-window class registration failed (%lu)\n", GetLastError());
        return 8;
    }
    hidden_window = CreateWindowExW(0, window_class.lpszClassName, L"AC1 D3D9 smoke",
                                    WS_OVERLAPPED, 0, 0, 640, 480, nullptr, nullptr,
                                    window_class.hInstance, nullptr);
    if (!hidden_window) {
        std::fprintf(stderr, "hidden-window creation failed (%lu)\n", GetLastError());
        return 9;
    }

    const uint32_t present_address = allocate_guest(nullptr, 56, 4);
    const uint32_t device_output = allocate_guest(nullptr, 4, 4);
    GuestPresentParameters present{};
    present.back_buffer_width = 640;
    present.back_buffer_height = 480;
    present.back_buffer_count = 1;
    present.swap_effect = 1; /* D3DSWAPEFFECT_DISCARD */
    present.device_window = 0x1234u;
    present.windowed = 1;
    present.enable_auto_depth_stencil = 1;
    present.auto_depth_stencil_format = 75; /* D3DFMT_D24S8 */
    present.presentation_interval = 0; /* D3DPRESENT_INTERVAL_DEFAULT */
    std::memcpy(resolve_guest(nullptr, present_address, sizeof(present), AC1_D3D9_GUEST_WRITE),
                &present, sizeof(present));
    const uint32_t create_args[] = {
        d3d9, 0, 1, 0x1234u, 0x20u, present_address, device_output
    };
    uint32_t create_result = 0;
    if (ac1_d3d9_dispatch(0x7e001000u + 16 * 0x10u, create_args, 7,
                          &create_result) != 1) {
        std::fprintf(stderr, "CreateDevice dispatch failed: %s\n", ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 10;
    }
    const uint32_t device = guest_word(device_output);
    if (FAILED(static_cast<HRESULT>(create_result)) || !device) {
        /* Keep the native smoke non-invasive, then exercise the proxy with fake COM. */
        std::printf("windowed CreateDevice unavailable (HRESULT=0x%08x); using fake COM surface smoke\n",
                    create_result);
        ac1_d3d9_shutdown();
        const bool fake_passed = run_fake_surface_smoke(callbacks);
        DestroyWindow(hidden_window);
        return fake_passed ? 0 : 19;
    }

    const uint32_t get_render_target = object_slot(device, 38);
    const uint32_t get_depth_stencil = object_slot(device, 40);
    const uint32_t render_output = allocate_guest(nullptr, 4, 4);
    const uint32_t depth_output = allocate_guest(nullptr, 4, 4);
    uint32_t getter_result = 0;
    const uint32_t render_args[] = {device, 0, render_output};
    const uint32_t depth_args[] = {device, depth_output};
    if (ac1_d3d9_argument_count(get_render_target) != 3 ||
        ac1_d3d9_argument_count(get_depth_stencil) != 2 ||
        ac1_d3d9_dispatch(get_render_target, render_args, 3, &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result)) || !guest_word(render_output) ||
        ac1_d3d9_dispatch(get_depth_stencil, depth_args, 2, &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result)) || !guest_word(depth_output)) {
        std::fprintf(stderr, "device surface getter smoke failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 11;
    }

    const uint32_t surface = guest_word(render_output);
    const uint32_t surface_desc_token = object_slot(surface, 12);
    const uint32_t desc_address = allocate_guest(nullptr, 32, 4);
    const uint32_t desc_args[] = {surface, desc_address};
    if (ac1_d3d9_argument_count(surface_desc_token) != 2 ||
        ac1_d3d9_dispatch(surface_desc_token, desc_args, 2, &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result))) {
        std::fprintf(stderr, "surface GetDesc smoke failed: %s (HRESULT=0x%08x)\n",
                     ac1_d3d9_last_error(), getter_result);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 12;
    }
    uint32_t surface_type = 0, surface_width = 0, surface_height = 0;
    std::memcpy(&surface_type,
                resolve_guest(nullptr, desc_address + 4, 4, AC1_D3D9_GUEST_READ), 4);
    std::memcpy(&surface_width,
                resolve_guest(nullptr, desc_address + 24, 4, AC1_D3D9_GUEST_READ), 4);
    std::memcpy(&surface_height,
                resolve_guest(nullptr, desc_address + 28, 4, AC1_D3D9_GUEST_READ), 4);
    if (surface_type != D3DRTYPE_SURFACE || !surface_width || !surface_height) {
        std::fprintf(stderr, "surface GetDesc returned unexpected type/extent (%u, %u x %u)\n",
                     surface_type, surface_width, surface_height);
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 13;
    }

    const uint8_t iid_unknown[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
    };
    const uint32_t iid_address = allocate_guest(nullptr, sizeof(iid_unknown), 4);
    const uint32_t queried_surface_output = allocate_guest(nullptr, 4, 4);
    std::memcpy(resolve_guest(nullptr, iid_address, sizeof(iid_unknown), AC1_D3D9_GUEST_WRITE),
                iid_unknown, sizeof(iid_unknown));
    const uint32_t query_surface_args[] = {surface, iid_address, queried_surface_output};
    if (ac1_d3d9_argument_count(object_slot(surface, 0)) != 3 ||
        ac1_d3d9_dispatch(object_slot(surface, 0), query_surface_args, 3,
                          &getter_result) != 1 ||
        FAILED(static_cast<HRESULT>(getter_result)) ||
        guest_word(queried_surface_output) != surface) {
        std::fprintf(stderr, "surface IUnknown QueryInterface identity failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 14;
    }

    const uint32_t surface_addref = object_slot(surface, 1);
    const uint32_t surface_release = object_slot(surface, 2);
    const uint32_t surface_this[] = {surface};
    if (ac1_d3d9_dispatch(surface_addref, surface_this, 1, &references) != 1 ||
        references != 3 ||
        ac1_d3d9_dispatch(surface_release, surface_this, 1, &references) != 1 ||
        references != 2 ||
        ac1_d3d9_dispatch(surface_release, surface_this, 1, &references) != 1 ||
        references != 1 ||
        ac1_d3d9_dispatch(surface_release, surface_this, 1, &references) != 1 ||
        references != 0) {
        std::fprintf(stderr, "surface COM reference smoke failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 15;
    }

    const uint32_t depth_surface = guest_word(depth_output);
    const uint32_t depth_release = object_slot(depth_surface, 2);
    const uint32_t depth_this[] = {depth_surface};
    if (ac1_d3d9_dispatch(depth_release, depth_this, 1, &references) != 1 || references != 0) {
        std::fprintf(stderr, "depth surface release smoke failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 16;
    }
    const uint32_t device_release = object_slot(device, 2);
    const uint32_t device_this[] = {device};
    if (ac1_d3d9_dispatch(device_release, device_this, 1, &references) != 1) {
        std::fprintf(stderr, "device release smoke failed: %s\n", ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 17;
    }
    if (ac1_d3d9_dispatch(release_token, this_args, 1, &remaining) != 1 || remaining != 0) {
        std::fprintf(stderr, "final D3D9 Release dispatch failed: %s\n",
                     ac1_d3d9_last_error());
        DestroyWindow(hidden_window);
        ac1_d3d9_shutdown();
        return 18;
    }
    DestroyWindow(hidden_window);
    ac1_d3d9_shutdown();
    std::printf("native D3D9 bridge smoke passed; adapters=%u, windowed device surface=%ux%u\n",
                adapter_count, surface_width, surface_height);
    return 0;
}
