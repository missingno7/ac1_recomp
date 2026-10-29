#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../ac1_dinput8_bridge.cpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t kGuestBase = 0x10000000u;
constexpr size_t kGuestMemorySize = 1024u;
constexpr uint32_t kFakeObject = kGuestBase + 0x100u;
constexpr uint32_t kStateBuffer = kGuestBase + 0x200u;
std::array<uint8_t, kGuestMemorySize> g_guest{};
std::array<void *, 32u> g_fake_vtable{};
struct FakeDevice { void **vtable; } g_fake_device{g_fake_vtable.data()};
HRESULT g_state_hresult = S_OK;
HRESULT g_acquire_hresult = DIERR_INPUTLOST;
uint32_t g_state_calls = 0u;
uint32_t g_acquire_calls = 0u;
uint32_t g_release_calls = 0u;
DWORD g_observed_size = 0u;
void *g_observed_buffer = nullptr;
bool g_allow_write = true;

void *resolve_guest(void *, uint32_t address, uint32_t size, uint32_t access)
{
    if (!size || (access != AC1_DINPUT8_GUEST_READ &&
                  access != AC1_DINPUT8_GUEST_WRITE) ||
        (access == AC1_DINPUT8_GUEST_WRITE && !g_allow_write) ||
        address < kGuestBase)
        return nullptr;
    const uint64_t offset = static_cast<uint64_t>(address) - kGuestBase;
    if (offset + size > g_guest.size()) return nullptr;
    return g_guest.data() + static_cast<size_t>(offset);
}

uint32_t guest_alloc(void *, uint32_t, uint32_t) { return 0u; }
void guest_free(void *, uint32_t) {}
uintptr_t guest_window(void *, uint32_t) { return 0u; }
uintptr_t guest_instance(void *, uint32_t) { return 0u; }

ULONG STDMETHODCALLTYPE fake_add_ref(IUnknown *) { return 1u; }
ULONG STDMETHODCALLTYPE fake_release(IUnknown *)
{
    ++g_release_calls;
    return 0u;
}

HRESULT STDMETHODCALLTYPE fake_get_device_state(IDirectInputDevice8W *, DWORD bytes,
                                                  LPVOID output)
{
    ++g_state_calls;
    g_observed_size = bytes;
    g_observed_buffer = output;
    if (SUCCEEDED(g_state_hresult) && output) {
        auto *bytes_out = static_cast<uint8_t *>(output);
        for (DWORD i = 0; i < bytes; ++i)
            bytes_out[i] = static_cast<uint8_t>(0xa0u + (i & 0x3fu));
    }
    return g_state_hresult;
}

HRESULT STDMETHODCALLTYPE fake_acquire(IDirectInputDevice8W *)
{
    ++g_acquire_calls;
    return g_acquire_hresult;
}

template <typename Function>
void *function_address(Function function)
{
    static_assert(sizeof(function) == sizeof(void *), "Windows function pointer ABI mismatch");
    void *address = nullptr;
    std::memcpy(&address, &function, sizeof(address));
    return address;
}

bool check(bool condition, const char *message)
{
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

uint32_t device_token(uint32_t slot)
{
    return AC1_DINPUT8_TOKEN_BASE +
        static_cast<uint32_t>(ObjectKind::DeviceW) * kKindStride + slot * kSlotStride;
}

} // namespace

int main()
{
    g_fake_vtable[1] = function_address(&fake_add_ref);
    g_fake_vtable[2] = function_address(&fake_release);
    g_fake_vtable[7] = function_address(&fake_acquire);
    g_fake_vtable[9] = function_address(&fake_get_device_state);

    ac1_dinput8_callbacks callbacks{};
    callbacks.struct_size = sizeof(callbacks);
    callbacks.guest_resolve = &resolve_guest;
    callbacks.guest_alloc = &guest_alloc;
    callbacks.guest_free = &guest_free;
    callbacks.guest_hwnd_to_host = &guest_window;
    callbacks.guest_hinstance_to_host = &guest_instance;
    if (!check(ac1_dinput8_configure(&callbacks, nullptr) == 1,
               "configure isolated fake-COM guest memory"))
        return 1;

    GuestObject object{};
    object.native = reinterpret_cast<IUnknown *>(&g_fake_device);
    object.kind = ObjectKind::DeviceW;
    object.guest_references = 1u;
    if (!check(g_objects.emplace(kFakeObject, object).second,
               "register fake W device proxy"))
        return 1;

    const uint32_t state_token = device_token(9u);
    const uint32_t acquire_token = device_token(7u);
    if (!check(ac1_dinput8_argument_count(state_token) == 3,
               "GetDeviceState arity includes this, cbData, and output") ||
        !check(ac1_dinput8_argument_count(acquire_token) == 1,
               "Acquire arity includes this only") ||
        !check(ac1_dinput8_argument_count(device_token(8u)) ==
                   AC1_DINPUT8_METHOD_UNSUPPORTED,
               "Unacquire remains unsupported until observed")) {
        ac1_dinput8_shutdown();
        return 1;
    }

    constexpr uint32_t kByteCount = 0x14u;
    const uint32_t state_args[] = {kFakeObject, kByteCount, kStateBuffer};
    uint32_t result = 0xdeadbeefu;
    g_guest.fill(0xccu);
    g_state_hresult = S_OK;
    const int status = ac1_dinput8_dispatch(state_token, state_args, 3u, &result);
    if (!check(status == AC1_DINPUT8_HANDLED && result == static_cast<uint32_t>(S_OK),
               "successful native GetDeviceState HRESULT is returned") ||
        !check(g_state_calls == 1u && g_observed_size == kByteCount,
               "native GetDeviceState receives exact cbData") ||
        !check(g_observed_buffer == resolve_guest(nullptr, kStateBuffer, kByteCount,
                                                   AC1_DINPUT8_GUEST_WRITE),
               "native output pointer is the fully validated guest range")) {
        ac1_dinput8_shutdown();
        return 1;
    }
    for (uint32_t i = 0; i < kByteCount; ++i) {
        if (!check(g_guest[0x200u + i] == static_cast<uint8_t>(0xa0u + (i & 0x3fu)),
                   "native-written bytes are visible in guest memory")) {
            ac1_dinput8_shutdown();
            return 1;
        }
    }

    constexpr uint32_t kKeyboardByteCount = 0x100u;
    const uint32_t keyboard_state_args[] = {
        kFakeObject, kKeyboardByteCount, kStateBuffer
    };
    g_state_hresult = S_OK;
    result = 0xdeadbeefu;
    if (!check(ac1_dinput8_dispatch(state_token, keyboard_state_args, 3u, &result) ==
                   AC1_DINPUT8_HANDLED && result == static_cast<uint32_t>(S_OK) &&
                   g_observed_size == kKeyboardByteCount,
               "keyboard GetDeviceState preserves its observed 0x100-byte request") ||
        !check(g_guest[0x200u] == 0xa0u &&
                   g_guest[0x200u + kKeyboardByteCount - 1u] == 0xdfu,
               "keyboard-sized native output is fully visible in guest memory")) {
        ac1_dinput8_shutdown();
        return 1;
    }

    /* A real native HRESULT failure is handled and preserved, without a fake
       success or clearing the caller's output buffer. */
    g_guest.fill(0x5au);
    g_state_hresult = DIERR_NOTACQUIRED;
    result = 0xdeadbeefu;
    if (!check(ac1_dinput8_dispatch(state_token, state_args, 3u, &result) ==
                   AC1_DINPUT8_HANDLED && result == static_cast<uint32_t>(DIERR_NOTACQUIRED),
               "DIERR_NOTACQUIRED is returned as the native failure HRESULT") ||
        !check(g_guest[0x200u] == 0x5au && g_guest[0x200u + kByteCount - 1u] == 0x5au,
               "failed GetDeviceState leaves native output semantics intact")) {
        ac1_dinput8_shutdown();
        return 1;
    }

    /* Invalid full spans fail before native code and leave result_out intact. */
    const uint32_t invalid_args[] = {
        kFakeObject, kByteCount,
        kGuestBase + static_cast<uint32_t>(kGuestMemorySize - 10u)
    };
    result = 0xdeadbeefu;
    const uint32_t calls_before_invalid = g_state_calls;
    if (!check(ac1_dinput8_dispatch(state_token, invalid_args, 3u, &result) ==
                   AC1_DINPUT8_ERROR && result == 0xdeadbeefu,
               "truncated guest output range returns bridge error") ||
        !check(g_state_calls == calls_before_invalid,
               "invalid guest output range is rejected before native GetDeviceState")) {
        ac1_dinput8_shutdown();
        return 1;
    }
    g_allow_write = false;
    result = 0xdeadbeefu;
    if (!check(ac1_dinput8_dispatch(state_token, state_args, 3u, &result) ==
                   AC1_DINPUT8_ERROR && result == 0xdeadbeefu,
               "read-only guest output range returns bridge error") ||
        !check(g_state_calls == calls_before_invalid,
               "nonwritable output is rejected before native call")) {
        g_allow_write = true;
        ac1_dinput8_shutdown();
        return 1;
    }
    g_allow_write = true;

    result = 0xdeadbeefu;
    g_acquire_hresult = DIERR_INPUTLOST;
    if (!check(ac1_dinput8_dispatch(acquire_token, &kFakeObject, 1u, &result) ==
                   AC1_DINPUT8_HANDLED && result == static_cast<uint32_t>(DIERR_INPUTLOST) &&
                   g_acquire_calls == 1u,
               "native Acquire error is preserved for the guest retry branch")) {
        ac1_dinput8_shutdown();
        return 1;
    }

    ac1_dinput8_shutdown();
    return check(g_release_calls == 1u, "fake COM object is released during shutdown") ? 0 : 1;
}
