#define DIRECTINPUT_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../ac1_dinput8_bridge.h"

#include <windows.h>
#include <dinput.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t kGuestBase = 0x10000000u;
std::array<uint8_t, 1024u * 1024u> g_guest{};
uint32_t g_next_allocation = kGuestBase + 0x80000u;
HWND g_test_window = nullptr;

#pragma pack(push, 4)
struct TestGuestDataFormat {
    uint32_t size;
    uint32_t object_size;
    uint32_t flags;
    uint32_t data_size;
    uint32_t object_count;
    uint32_t objects;
};
struct TestGuestObjectDataFormat {
    uint32_t guid;
    uint32_t offset;
    uint32_t type;
    uint32_t flags;
};
#pragma pack(pop)

void *resolve(void *, uint32_t address, uint32_t size, uint32_t access)
{
    if (!size || (access != AC1_DINPUT8_GUEST_READ &&
                  access != AC1_DINPUT8_GUEST_WRITE) || address < kGuestBase)
        return nullptr;
    const uint64_t offset = static_cast<uint64_t>(address) - kGuestBase;
    if (offset + size > g_guest.size()) return nullptr;
    return g_guest.data() + static_cast<size_t>(offset);
}

uint32_t allocate(void *, uint32_t size, uint32_t alignment)
{
    if (!size || !alignment || (alignment & (alignment - 1u))) return 0u;
    const uint32_t mask = alignment - 1u;
    g_next_allocation = (g_next_allocation + mask) & ~mask;
    const uint32_t address = g_next_allocation;
    if (static_cast<uint64_t>(address) + size >
        static_cast<uint64_t>(kGuestBase) + g_guest.size())
        return 0u;
    g_next_allocation += size;
    return address;
}

void deallocate(void *, uint32_t) {}

uintptr_t map_hwnd(void *, uint32_t guest_hwnd)
{
    return guest_hwnd == 0x1234u
        ? reinterpret_cast<uintptr_t>(g_test_window) : 0u;
}

uintptr_t map_hinstance(void *, uint32_t guest_hinstance)
{
    if (guest_hinstance != 0x00400000u) return 0u;
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

bool check(bool condition, const char *message)
{
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

template <typename T>
bool put(uint32_t address, const T &value)
{
    void *destination = resolve(nullptr, address, sizeof(T), AC1_DINPUT8_GUEST_WRITE);
    if (!destination) return false;
    std::memcpy(destination, &value, sizeof(T));
    return true;
}

template <typename T>
bool get(uint32_t address, T *value)
{
    void *source = resolve(nullptr, address, sizeof(T), AC1_DINPUT8_GUEST_READ);
    if (!source || !value) return false;
    std::memcpy(value, source, sizeof(T));
    return true;
}

HWND make_hidden_window()
{
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"AC1DirectInputBridgeSmokeWindow";
    if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return nullptr;
    return CreateWindowExW(0, window_class.lpszClassName, L"AC1 DirectInput smoke",
                           WS_OVERLAPPED, 0, 0, 320, 200, nullptr, nullptr,
                           window_class.hInstance, nullptr);
}

} // namespace

int main()
{
    ac1_dinput8_callbacks callbacks{};
    callbacks.struct_size = sizeof(callbacks);
    callbacks.guest_resolve = resolve;
    callbacks.guest_alloc = allocate;
    callbacks.guest_free = deallocate;
    callbacks.guest_hwnd_to_host = map_hwnd;
    callbacks.guest_hinstance_to_host = map_hinstance;
    if (!check(ac1_dinput8_configure(&callbacks, nullptr) == 1,
               "configure guest-memory callbacks"))
        return 1;

    constexpr uint32_t iid_address = kGuestBase + 0x100u;
    constexpr uint32_t output_address = kGuestBase + 0x200u;
    const GUID iid_input_w =
        {0xBF798031u, 0x483Au, 0x4DA2u, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
    if (!check(put(iid_address, iid_input_w), "write guest IID")) return 1;

    uint32_t native_hr = 0u;
    const int create_status = ac1_dinput8_create8(
        0x00400000u, 0x0800u, iid_address, output_address, 0u, &native_hr);
    if (!check(create_status == AC1_DINPUT8_HANDLED,
               ac1_dinput8_last_error()[0] ? ac1_dinput8_last_error()
                                           : "DirectInput8Create native call"))
        return 1;
    if (!check(static_cast<int32_t>(native_hr) >= 0,
               "native DirectInput8Create returned success")) {
        std::fprintf(stderr, "HRESULT=0x%08x\n", native_hr);
        ac1_dinput8_shutdown();
        return 1;
    }

    uint32_t guest_input = 0u;
    if (!check(get(output_address, &guest_input) && guest_input != 0u,
               "factory returned guest proxy, not host COM pointer")) {
        ac1_dinput8_shutdown();
        return 1;
    }
    const uint32_t create_device_token =
        AC1_DINPUT8_TOKEN_BASE + 3u * 0x1000u + 3u * 0x10u;
    const uint32_t enum_devices_token =
        AC1_DINPUT8_TOKEN_BASE + 3u * 0x1000u + 4u * 0x10u;
    const uint32_t enum_objects_token =
        AC1_DINPUT8_TOKEN_BASE + 5u * 0x1000u + 4u * 0x10u;
    if (!check(ac1_dinput8_argument_count(create_device_token) == 4,
               "CreateDevice x86 arity includes this and three parameters") ||
        !check(ac1_dinput8_argument_count(enum_devices_token) == 5,
               "W EnumDevices x86 arity includes this and four parameters") ||
        !check(ac1_dinput8_argument_count(enum_objects_token) == 4,
               "W EnumObjects x86 arity includes this, callback, context, and flags") ||
        !check(ac1_dinput8_argument_count(0x12345678u) == AC1_DINPUT8_NOT_HANDLED,
               "foreign dispatch token is not claimed")) {
        ac1_dinput8_shutdown();
        return 1;
    }
    ac1_dinput8_enum_batch empty_batch{};
    if (!check(ac1_dinput8_enum_batch_take(&empty_batch) == AC1_DINPUT8_NOT_HANDLED,
               "no unrequested EnumObjects records are fabricated")) {
        ac1_dinput8_shutdown();
        return 1;
    }

    const uint32_t enum_devices_args[] = {
        guest_input, DI8DEVCLASS_ALL, 0x009165C0u, kGuestBase + 0x950u,
        DIEDFL_ATTACHEDONLY
    };
    uint32_t enum_devices_hr = 0u;
    if (!check(ac1_dinput8_dispatch(enum_devices_token, enum_devices_args, 5u,
                                    &enum_devices_hr) == AC1_DINPUT8_HANDLED,
               "native W EnumDevices dispatch") ||
        !check(static_cast<int32_t>(enum_devices_hr) >= 0,
               "native W EnumDevices returned success")) {
        std::fprintf(stderr, "EnumDevices HRESULT=0x%08x\n", enum_devices_hr);
        ac1_dinput8_shutdown();
        return 1;
    }
    ac1_dinput8_enum_batch device_batch{};
    if (!check(ac1_dinput8_enum_batch_take(&device_batch) == AC1_DINPUT8_HANDLED,
               "take native EnumDevices records") ||
        !check(device_batch.native_hresult == static_cast<int32_t>(enum_devices_hr),
               "EnumDevices batch retains the native HRESULT") ||
        !check(device_batch.record_size == 0x44cu && device_batch.count > 0u,
               "native attached-device enumeration is nonempty with 0x44c W records")) {
        ac1_dinput8_enum_batch_free(&device_batch);
        ac1_dinput8_shutdown();
        return 1;
    }
    const auto *first_device = static_cast<const DIDEVICEINSTANCEW *>(
        ac1_dinput8_enum_batch_record(&device_batch, 0u));
    if (!check(first_device && first_device->dwSize == 0x44cu,
               "EnumDevices record has the canonical full W layout")) {
        ac1_dinput8_enum_batch_free(&device_batch);
        ac1_dinput8_shutdown();
        return 1;
    }
    ac1_dinput8_enum_batch_free(&device_batch);

    const uint32_t game_controller_args[] = {
        guest_input, DI8DEVCLASS_GAMECTRL, 0x009165C0u, kGuestBase + 0x980u,
        DIEDFL_ATTACHEDONLY
    };
    uint32_t enum_controllers_hr = 0u;
    if (!check(ac1_dinput8_dispatch(enum_devices_token, game_controller_args, 5u,
                                    &enum_controllers_hr) == AC1_DINPUT8_HANDLED,
               "native attached game-controller EnumDevices dispatch") ||
        !check(static_cast<int32_t>(enum_controllers_hr) >= 0,
               "native attached game-controller EnumDevices returned success")) {
        std::fprintf(stderr, "EnumDevices game controllers HRESULT=0x%08x\n",
                     enum_controllers_hr);
        ac1_dinput8_shutdown();
        return 1;
    }
    ac1_dinput8_enum_batch controller_batch{};
    if (!check(ac1_dinput8_enum_batch_take(&controller_batch) == AC1_DINPUT8_HANDLED,
               "take native game-controller enumeration results") ||
        !check(controller_batch.native_hresult == static_cast<int32_t>(enum_controllers_hr) &&
                   controller_batch.record_size == 0x44cu,
               "controller enumeration preserves native HRESULT and W record ABI")) {
        ac1_dinput8_enum_batch_free(&controller_batch);
        ac1_dinput8_shutdown();
        return 1;
    }
    /* A count of zero is valid only when returned by the real native provider. */
    ac1_dinput8_enum_batch_free(&controller_batch);

    uint32_t addref_result = 0u;
    const uint32_t addref_token = AC1_DINPUT8_TOKEN_BASE +
        3u * 0x1000u + 1u * 0x10u;
    const uint32_t addref_args[] = {guest_input};
    if (!check(ac1_dinput8_dispatch(addref_token, addref_args, 1u,
                                   &addref_result) == AC1_DINPUT8_HANDLED,
               "native COM AddRef dispatch")) {
        ac1_dinput8_shutdown();
        return 1;
    }

    const GUID sys_keyboard =
        {0x6F1D2B61u, 0xD5A0u, 0x11CFu, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    constexpr uint32_t guid_address = kGuestBase + 0x300u;
    constexpr uint32_t device_output = kGuestBase + 0x320u;
    if (!check(put(guid_address, sys_keyboard), "write guest keyboard GUID")) {
        ac1_dinput8_shutdown();
        return 1;
    }
    const uint32_t create_args[] = {guest_input, guid_address, device_output, 0u};
    uint32_t create_device_hr = 0u;
    const int create_device_status = ac1_dinput8_dispatch(
        create_device_token, create_args, 4u, &create_device_hr);
    if (!check(create_device_status == AC1_DINPUT8_HANDLED,
               ac1_dinput8_last_error()[0] ? ac1_dinput8_last_error()
                                           : "native CreateDevice dispatch")) {
        ac1_dinput8_shutdown();
        return 1;
    }
    uint32_t guest_device = 0u;
    if (static_cast<int32_t>(create_device_hr) >= 0) {
        if (!check(get(device_output, &guest_device) && guest_device != 0u,
                   "native CreateDevice returned a guest device proxy")) {
            ac1_dinput8_shutdown();
            return 1;
        }

        const uint32_t enum_args[] = {
            guest_device, 0x009091C0u, kGuestBase + 0x900u, 0u
        };
        uint32_t enum_hr = 0u;
        if (!check(ac1_dinput8_dispatch(enum_objects_token, enum_args, 4u,
                                        &enum_hr) == AC1_DINPUT8_HANDLED,
                   "native W EnumObjects dispatch")) {
            ac1_dinput8_shutdown();
            return 1;
        }
        ac1_dinput8_enum_batch batch{};
        if (!check(ac1_dinput8_enum_batch_take(&batch) == AC1_DINPUT8_HANDLED,
                   "take native EnumObjects records") ||
            !check(static_cast<int32_t>(enum_hr) >= 0,
                   "native W EnumObjects returned success") ||
            !check(batch.record_size == 0x240u && batch.count > 0u,
                   "EnumObjects returned nonempty 0x240-byte W records")) {
            ac1_dinput8_enum_batch_free(&batch);
            ac1_dinput8_shutdown();
            return 1;
        }
        const GUID key_guid =
            {0x55728220u, 0xD33Cu, 0x11CFu, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
        bool found_key = false;
        for (uint32_t i = 0; i < batch.count; ++i) {
            const auto *record = static_cast<const DIDEVICEOBJECTINSTANCEW *>(
                ac1_dinput8_enum_batch_record(&batch, i));
            if (record && record->dwSize == 0x240u &&
                IsEqualGUID(record->guidType, key_guid) &&
                record->tszName[0] != L'\0') {
                found_key = true;
                break;
            }
        }
        if (!check(found_key, "keyboard enumeration includes a wide-named key object")) {
            ac1_dinput8_enum_batch_free(&batch);
            ac1_dinput8_shutdown();
            return 1;
        }
        ac1_dinput8_enum_batch_free(&batch);

        constexpr uint32_t key_guid_address = kGuestBase + 0x500u;
        constexpr uint32_t format_address = kGuestBase + 0x600u;
        constexpr uint32_t objects_address = kGuestBase + 0x700u;
        constexpr uint32_t format_token = AC1_DINPUT8_TOKEN_BASE +
            5u * 0x1000u + 11u * 0x10u;
        if (!check(put(key_guid_address, key_guid), "write guest keyboard object GUID")) {
            ac1_dinput8_shutdown();
            return 1;
        }
        std::array<TestGuestObjectDataFormat, 256> format_objects{};
        for (uint32_t i = 0; i < format_objects.size(); ++i) {
            format_objects[i].guid = key_guid_address;
            format_objects[i].offset = i;
            format_objects[i].type = DIDFT_BUTTON | DIDFT_MAKEINSTANCE(i) | DIDFT_OPTIONAL;
        }
        TestGuestDataFormat data_format{
            sizeof(TestGuestDataFormat), sizeof(TestGuestObjectDataFormat),
            DIDF_RELAXIS, 0x100u, static_cast<uint32_t>(format_objects.size()),
            objects_address
        };
        if (!check(put(format_address, data_format), "write guest DIDATAFORMAT") ||
            !check(put(objects_address, format_objects),
                   "write guest 256-entry keyboard format")) {
            ac1_dinput8_shutdown();
            return 1;
        }
        const uint32_t format_args[] = {guest_device, format_address};
        uint32_t format_hr = 0u;
        if (!check(ac1_dinput8_argument_count(format_token) == 2,
                   "SetDataFormat x86 arity") ||
            !check(ac1_dinput8_dispatch(format_token, format_args, 2u,
                                        &format_hr) == AC1_DINPUT8_HANDLED,
                   ac1_dinput8_last_error()[0] ? ac1_dinput8_last_error()
                                               : "native keyboard SetDataFormat dispatch") ||
            !check(static_cast<int32_t>(format_hr) >= 0,
                   "native keyboard SetDataFormat succeeded")) {
            std::fprintf(stderr, "SetDataFormat HRESULT=0x%08x\n", format_hr);
            ac1_dinput8_shutdown();
            return 1;
        }

        constexpr uint32_t cooperative_token = AC1_DINPUT8_TOKEN_BASE +
            5u * 0x1000u + 13u * 0x10u;
        if (!check(ac1_dinput8_argument_count(cooperative_token) == 3,
                   "SetCooperativeLevel x86 arity")) {
            ac1_dinput8_shutdown();
            return 1;
        }
        const uint32_t null_cooperative_args[] = {guest_device, 0u, 6u};
        uint32_t cooperative_hr = 0u;
        if (!check(ac1_dinput8_dispatch(cooperative_token, null_cooperative_args,
                                        3u, &cooperative_hr) == AC1_DINPUT8_HANDLED,
                   ac1_dinput8_last_error()[0] ? ac1_dinput8_last_error()
                                               : "native null-HWND SetCooperativeLevel")) {
            ac1_dinput8_shutdown();
            return 1;
        }
        std::printf("native null-HWND SetCooperativeLevel flags=0x6 HRESULT=0x%08x\n",
                    cooperative_hr);
        if (static_cast<int32_t>(cooperative_hr) >= 0) {
            const uint32_t null_keyboard_cooperative_args[] = {
                guest_device, 0u, 0x16u
            };
            if (!check(ac1_dinput8_dispatch(cooperative_token,
                                            null_keyboard_cooperative_args, 3u,
                                            &cooperative_hr) == AC1_DINPUT8_HANDLED,
                       "native null-HWND keyboard SetCooperativeLevel")) {
                ac1_dinput8_shutdown();
                return 1;
            }
            std::printf("native null-HWND SetCooperativeLevel flags=0x16 HRESULT=0x%08x\n",
                        cooperative_hr);
        }
        g_test_window = make_hidden_window();
        if (!check(g_test_window != nullptr, "create hidden HWND for mapping test")) {
            ac1_dinput8_shutdown();
            return 1;
        }
        const uint32_t mapped_cooperative_args[] = {guest_device, 0x1234u, 6u};
        if (!check(ac1_dinput8_dispatch(cooperative_token, mapped_cooperative_args,
                                        3u, &cooperative_hr) == AC1_DINPUT8_HANDLED,
                   ac1_dinput8_last_error()[0] ? ac1_dinput8_last_error()
                                               : "mapped-HWND SetCooperativeLevel")) {
            DestroyWindow(g_test_window);
            g_test_window = nullptr;
            ac1_dinput8_shutdown();
            return 1;
        }
        DestroyWindow(g_test_window);
        g_test_window = nullptr;

        const uint32_t unmapped_cooperative_args[] = {guest_device, 0x5678u, 6u};
        uint32_t result_sentinel = 0xdeadbeefu;
        if (!check(ac1_dinput8_dispatch(cooperative_token,
                                        unmapped_cooperative_args, 3u,
                                        &result_sentinel) == AC1_DINPUT8_ERROR &&
                       result_sentinel == 0xdeadbeefu,
                   "unmapped nonzero HWND reports bridge error without fake HRESULT")) {
            ac1_dinput8_shutdown();
            return 1;
        }

        const uint32_t device_release_token = AC1_DINPUT8_TOKEN_BASE +
            5u * 0x1000u + 2u * 0x10u;
        const uint32_t device_args[] = {guest_device};
        uint32_t device_release_result = 0u;
        if (!check(ac1_dinput8_dispatch(device_release_token, device_args, 1u,
                                        &device_release_result) == AC1_DINPUT8_HANDLED,
                   "native device Release dispatch")) {
            ac1_dinput8_shutdown();
            return 1;
        }
    } else {
        std::fprintf(stderr, "native keyboard CreateDevice returned HRESULT=0x%08x\n",
                     create_device_hr);
        ac1_dinput8_shutdown();
        return 1;
    }

    const GUID sys_mouse =
        {0x6F1D2B60u, 0xD5A0u, 0x11CFu, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    constexpr uint32_t mouse_guid_address = kGuestBase + 0x340u;
    constexpr uint32_t mouse_output_address = kGuestBase + 0x360u;
    if (!check(put(mouse_guid_address, sys_mouse), "write guest mouse GUID")) {
        ac1_dinput8_shutdown();
        return 1;
    }
    const uint32_t create_mouse_args[] = {
        guest_input, mouse_guid_address, mouse_output_address, 0u
    };
    uint32_t create_mouse_hr = 0u;
    if (!check(ac1_dinput8_dispatch(create_device_token, create_mouse_args, 4u,
                                    &create_mouse_hr) == AC1_DINPUT8_HANDLED,
               "native mouse CreateDevice dispatch") ||
        !check(static_cast<int32_t>(create_mouse_hr) >= 0,
               "native mouse CreateDevice succeeded")) {
        std::fprintf(stderr, "mouse CreateDevice HRESULT=0x%08x\n", create_mouse_hr);
        ac1_dinput8_shutdown();
        return 1;
    }
    uint32_t guest_mouse = 0u;
    if (!check(get(mouse_output_address, &guest_mouse) && guest_mouse != 0u,
               "native mouse CreateDevice returned guest proxy")) {
        ac1_dinput8_shutdown();
        return 1;
    }
    const uint32_t mouse_release_args[] = {guest_mouse};
    uint32_t mouse_release_result = 0u;
    const uint32_t device_release_token = AC1_DINPUT8_TOKEN_BASE +
        5u * 0x1000u + 2u * 0x10u;
    if (!check(ac1_dinput8_dispatch(device_release_token, mouse_release_args, 1u,
                                    &mouse_release_result) == AC1_DINPUT8_HANDLED,
               "native mouse Release dispatch")) {
        ac1_dinput8_shutdown();
        return 1;
    }
    uint32_t release_result = 0u;
    const uint32_t release_token = AC1_DINPUT8_TOKEN_BASE +
        3u * 0x1000u + 2u * 0x10u;
    if (!check(ac1_dinput8_dispatch(release_token, addref_args, 1u,
                                    &release_result) == AC1_DINPUT8_HANDLED,
               "native COM Release dispatch")) {
        ac1_dinput8_shutdown();
        return 1;
    }

    std::printf("DirectInput8Create succeeded; guest proxy=%08x; CreateDevice arity=%d\n",
                guest_input, ac1_dinput8_argument_count(create_device_token));
    ac1_dinput8_shutdown();
    return 0;
}
