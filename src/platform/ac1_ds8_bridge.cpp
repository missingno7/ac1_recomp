#define DIRECTSOUND_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#include "ac1_ds8_bridge.h"

#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kKindStride = 0x1000u;
constexpr uint32_t kSlotStride = 0x10u;
constexpr uint32_t kUnknownKind = 1u;
constexpr uint32_t kDeviceKind = 2u;
constexpr uint32_t kBufferKind = 3u;
constexpr uint32_t kListenerKind = 4u;
constexpr uint32_t kBuffer8Kind = 5u;
constexpr uint32_t kUnknownSlots = 3u;
constexpr uint32_t kDeviceSlots = 12u;
constexpr uint32_t kBufferSlots = 21u;
constexpr uint32_t kListenerSlots = 18u;
constexpr uint32_t kBuffer8Slots = 24u;

const GUID kIidUnknown =
    {0x00000000u, 0x0000u, 0x0000u, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID kIidDirectSound8 =
    {0xC50A7E93u, 0xF395u, 0x4834u, {0x9E, 0xF6, 0x7F, 0xA9, 0x9D, 0xE5, 0x09, 0x66}};
const GUID kIidDirectSoundBuffer =
    {0x279AFA85u, 0x4981u, 0x11CEu, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
const GUID kIidDirectSoundBuffer8 =
    {0x6825A449u, 0x7524u, 0x4D82u, {0x92, 0x0F, 0x50, 0xE3, 0x6A, 0xB3, 0xAB, 0x1E}};
const GUID kIidDirectSound3DListener =
    {0x279AFA84u, 0x4981u, 0x11CEu, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};

enum class ObjectKind : uint32_t {
    Unknown = kUnknownKind,
    Device = kDeviceKind,
    Buffer = kBufferKind,
    Listener = kListenerKind,
    Buffer8 = kBuffer8Kind
};

struct GuestObject {
    IUnknown *native = nullptr;
    ObjectKind kind = ObjectKind::Unknown;
    uint32_t guest_vtable = 0;
    uint32_t guest_references = 1;
    struct {
        bool active = false;
        void *host_pointer1 = nullptr;
        DWORD host_bytes1 = 0;
        void *host_pointer2 = nullptr;
        DWORD host_bytes2 = 0;
        uint32_t guest_pointer1 = 0;
        uint32_t guest_pointer2 = 0;
    } lock;
};

/* Guest DSBUFFERDESC is pack-4 x86 and contains a 32-bit format pointer plus
 * an inline GUID. Never copy this structure directly into the native x64 ABI. */
#pragma pack(push, 4)
struct GuestDsBufferDesc {
    uint32_t size;
    uint32_t flags;
    uint32_t buffer_bytes;
    uint32_t reserved;
    uint32_t wave_format;
    GUID algorithm;
};
#pragma pack(pop)
static_assert(sizeof(GuestDsBufferDesc) == 36u,
              "guest x86 DSBUFFERDESC must be 36 bytes");
static_assert(sizeof(WAVEFORMATEX) == 18u,
              "guest/native WAVEFORMATEX base must be 18 bytes");

ac1_ds8_callbacks g_callbacks{};
void *g_context = nullptr;
std::map<uint32_t, GuestObject> g_objects;
std::mutex g_mutex;
thread_local std::string g_last_error;
HMODULE g_dsound_module = nullptr;

void set_error(const std::string &message)
{
    g_last_error = message;
    if (g_callbacks.log) g_callbacks.log(g_context, message.c_str());
    else OutputDebugStringA(("AC1 DirectSound bridge: " + message + "\n").c_str());
}

void clear_error()
{
    g_last_error.clear();
}

void *guest_memory(uint32_t address, uint32_t size, uint32_t access)
{
    if (!address || !size || !g_callbacks.guest_resolve) return nullptr;
    return g_callbacks.guest_resolve(g_context, address, size, access);
}

bool guest_read(uint32_t address, void *output, uint32_t size)
{
    void *source = guest_memory(address, size, AC1_DS8_GUEST_READ);
    if (!source) return false;
    std::memcpy(output, source, size);
    return true;
}

bool guest_write(uint32_t address, const void *input, uint32_t size)
{
    void *destination = guest_memory(address, size, AC1_DS8_GUEST_WRITE);
    if (!destination) return false;
    std::memcpy(destination, input, size);
    return true;
}

bool is_buffer_kind(ObjectKind kind)
{
    return kind == ObjectKind::Buffer || kind == ObjectKind::Buffer8;
}

IDirectSoundBuffer *native_buffer(GuestObject &object)
{
    return reinterpret_cast<IDirectSoundBuffer *>(object.native);
}

void free_lock_shadows(GuestObject &object)
{
    if (object.lock.guest_pointer1 && g_callbacks.guest_free)
        g_callbacks.guest_free(g_context, object.lock.guest_pointer1);
    if (object.lock.guest_pointer2 && g_callbacks.guest_free)
        g_callbacks.guest_free(g_context, object.lock.guest_pointer2);
    object.lock.guest_pointer1 = 0;
    object.lock.guest_pointer2 = 0;
}

HRESULT unlock_native_buffer(GuestObject &object)
{
    if (!is_buffer_kind(object.kind) || !object.lock.active) return S_OK;
    const HRESULT result = native_buffer(object)->Unlock(
        object.lock.host_pointer1, object.lock.host_bytes1,
        object.lock.host_pointer2, object.lock.host_bytes2);
    if (SUCCEEDED(result)) {
        free_lock_shadows(object);
        object.lock = {};
    }
    return result;
}

uint32_t slot_token(ObjectKind kind, uint32_t slot)
{
    return AC1_DS8_TOKEN_BASE + static_cast<uint32_t>(kind) * kKindStride +
           slot * kSlotStride;
}

uint32_t slot_count(ObjectKind kind)
{
    switch (kind) {
    case ObjectKind::Unknown: return kUnknownSlots;
    case ObjectKind::Device: return kDeviceSlots;
    case ObjectKind::Buffer: return kBufferSlots;
    case ObjectKind::Listener: return kListenerSlots;
    case ObjectKind::Buffer8: return kBuffer8Slots;
    }
    return 0;
}

int token_argument_count(uint32_t token)
{
    if (token < AC1_DS8_TOKEN_BASE + kKindStride ||
        token >= AC1_DS8_TOKEN_BASE + 6u * kKindStride)
        return 0;
    const uint32_t offset = token - AC1_DS8_TOKEN_BASE;
    const uint32_t kind = offset / kKindStride;
    const uint32_t within_kind = offset % kKindStride;
    if (kind < kUnknownKind || kind > kBuffer8Kind ||
        within_kind % kSlotStride)
        return -1;

    const uint32_t method = within_kind / kSlotStride;
    const ObjectKind object_kind = static_cast<ObjectKind>(kind);
    if (method >= slot_count(object_kind)) return -1;

    if (method == 0u) return 3; /* QueryInterface(this, iid, output). */
    if (method == 1u || method == 2u) return 1; /* AddRef / Release. */
    if (object_kind == ObjectKind::Device && method == 3u)
        return 4; /* CreateSoundBuffer(this, desc, output, outer). */
    if (object_kind == ObjectKind::Device && method == 6u)
        return 3; /* SetCooperativeLevel(this, HWND, priority). */
    if (object_kind == ObjectKind::Device && method == 8u)
        return 2; /* GetSpeakerConfig(this, DWORD*). */
    if (is_buffer_kind(object_kind) && method == 11u)
        return 8; /* Lock(this, offset, bytes, ptr1, size1, ptr2, size2, flags). */
    if (is_buffer_kind(object_kind) && method == 12u)
        return 4; /* Play(this, reserved1, reserved2, flags). */
    if (is_buffer_kind(object_kind) && method == 14u)
        return 2; /* SetFormat(this, WAVEFORMATEX*). */
    if (is_buffer_kind(object_kind) && method == 15u)
        return 2; /* SetVolume(this, LONG). */
    if (is_buffer_kind(object_kind) && method == 16u)
        return 2; /* SetPan(this, LONG). */
    if (is_buffer_kind(object_kind) && method == 19u)
        return 5; /* Unlock(this, ptr1, size1, ptr2, size2). */
    return -2;
}

bool ensure_dsound_module()
{
    if (g_dsound_module) return true;

    wchar_t system_directory[32768]{};
    SetLastError(ERROR_SUCCESS);
    const UINT length = GetSystemDirectoryW(
        system_directory,
        static_cast<UINT>(sizeof(system_directory) / sizeof(system_directory[0])));
    static constexpr wchar_t suffix[] = L"\\dsound.dll";
    const UINT capacity = static_cast<UINT>(sizeof(system_directory) /
                                            sizeof(system_directory[0]));
    if (!length) {
        set_error("GetSystemDirectoryW failed for System32 DirectSound");
        return false;
    }
    if (length >= capacity || length + (sizeof(suffix) / sizeof(suffix[0])) > capacity) {
        set_error("System32 path does not fit while loading DirectSound");
        return false;
    }
    std::memcpy(system_directory + length, suffix, sizeof(suffix));
    g_dsound_module = LoadLibraryW(system_directory);
    if (!g_dsound_module) {
        const DWORD code = GetLastError();
        std::ostringstream message;
        message << "System32 dsound.dll could not load (Win32 error " << code << ')';
        set_error(message.str());
        return false;
    }
    return true;
}

using DirectSoundCreate8Fn = HRESULT (WINAPI *)(LPCGUID, LPDIRECTSOUND8 *, LPUNKNOWN);

DirectSoundCreate8Fn find_create8()
{
    if (!ensure_dsound_module()) return nullptr;
    FARPROC raw = GetProcAddress(g_dsound_module, "DirectSoundCreate8");
    if (!raw) {
        const DWORD code = GetLastError();
        std::ostringstream message;
        message << "System32 DirectSoundCreate8 is unavailable (Win32 error " << code << ')';
        set_error(message.str());
        return nullptr;
    }
    static_assert(sizeof(DirectSoundCreate8Fn) == sizeof(FARPROC),
                  "Windows function-pointer ABI mismatch");
    DirectSoundCreate8Fn function = nullptr;
    std::memcpy(&function, &raw, sizeof(function));
    return function;
}

void free_guest_proxy(uint32_t address, const GuestObject &object)
{
    if (g_callbacks.guest_free) {
        if (object.guest_vtable) g_callbacks.guest_free(g_context, object.guest_vtable);
        if (address) g_callbacks.guest_free(g_context, address);
    }
}

uint32_t wrap_native(IUnknown *native, ObjectKind kind)
{
    if (!native) return 0;
    const uint32_t slots = slot_count(kind);
    const uint32_t table_bytes = slots * sizeof(uint32_t);
    const uint32_t table = g_callbacks.guest_alloc(g_context, table_bytes, 4u);
    if (!table) {
        set_error("guest allocation failed for DirectSound proxy vtable");
        native->Release();
        return 0;
    }
    const uint32_t address = g_callbacks.guest_alloc(g_context, sizeof(uint32_t), 4u);
    if (!address) {
        set_error("guest allocation failed for DirectSound proxy object");
        g_callbacks.guest_free(g_context, table);
        native->Release();
        return 0;
    }

    void *table_memory = guest_memory(table, table_bytes, AC1_DS8_GUEST_WRITE);
    if (!table_memory) {
        set_error("allocated DirectSound proxy vtable is not guest-writable");
        g_callbacks.guest_free(g_context, address);
        g_callbacks.guest_free(g_context, table);
        native->Release();
        return 0;
    }
    for (uint32_t slot = 0; slot < slots; ++slot) {
        const uint32_t token = slot_token(kind, slot);
        std::memcpy(static_cast<uint8_t *>(table_memory) + slot * sizeof(token),
                    &token, sizeof(token));
    }
    if (!guest_write(address, &table, sizeof(table))) {
        set_error("allocated DirectSound proxy object is not guest-writable");
        g_callbacks.guest_free(g_context, address);
        g_callbacks.guest_free(g_context, table);
        native->Release();
        return 0;
    }

    GuestObject object;
    object.native = native;
    object.kind = kind;
    object.guest_vtable = table;
    bool inserted = false;
    try {
        inserted = g_objects.emplace(address, object).second;
    } catch (...) {
        set_error("host allocation failed while tracking a DirectSound proxy");
        g_callbacks.guest_free(g_context, address);
        g_callbacks.guest_free(g_context, table);
        native->Release();
        return 0;
    }
    if (!inserted) {
        set_error("guest allocator reused a live DirectSound proxy address");
        g_callbacks.guest_free(g_context, address);
        g_callbacks.guest_free(g_context, table);
        native->Release();
        return 0;
    }
    return address;
}

bool iid_kind(const GUID &iid, ObjectKind *kind)
{
    if (!kind) return false;
    if (IsEqualGUID(iid, kIidUnknown)) *kind = ObjectKind::Unknown;
    else if (IsEqualGUID(iid, kIidDirectSound8)) *kind = ObjectKind::Device;
    else if (IsEqualGUID(iid, kIidDirectSoundBuffer)) *kind = ObjectKind::Buffer;
    else if (IsEqualGUID(iid, kIidDirectSoundBuffer8)) *kind = ObjectKind::Buffer8;
    else if (IsEqualGUID(iid, kIidDirectSound3DListener)) *kind = ObjectKind::Listener;
    else return false;
    return true;
}

int invoke_query_interface(GuestObject &object, const uint32_t *args,
                           uint32_t *result_out)
{
    GUID iid{};
    if (!guest_read(args[1], &iid, sizeof(iid))) {
        set_error("DirectSound QueryInterface IID is not readable in guest memory");
        return AC1_DS8_ERROR;
    }
    ObjectKind result_kind{};
    if (!iid_kind(iid, &result_kind)) {
        set_error("DirectSound QueryInterface requested an unimplemented interface");
        return AC1_DS8_METHOD_UNSUPPORTED;
    }
    uint32_t *guest_output = static_cast<uint32_t *>(
        guest_memory(args[2], sizeof(uint32_t), AC1_DS8_GUEST_WRITE));
    if (!guest_output) {
        set_error("DirectSound QueryInterface output is not writable in guest memory");
        return AC1_DS8_ERROR;
    }
    *guest_output = 0;

    void *native_result = nullptr;
    HRESULT result = object.native->QueryInterface(iid, &native_result);
    if (FAILED(result)) {
        if (native_result) static_cast<IUnknown *>(native_result)->Release();
        *result_out = static_cast<uint32_t>(result);
        return AC1_DS8_HANDLED;
    }
    if (!native_result) {
        set_error("DirectSound QueryInterface succeeded with a null COM interface");
        return AC1_DS8_ERROR;
    }
    const uint32_t guest_object = wrap_native(static_cast<IUnknown *>(native_result),
                                              result_kind);
    if (!guest_object) return AC1_DS8_ERROR;
    *guest_output = guest_object;
    *result_out = static_cast<uint32_t>(result);
    return AC1_DS8_HANDLED;
}

bool copy_guest_wave_format(uint32_t address, std::vector<uint8_t> *native_bytes,
                            const char *operation)
{
    if (!native_bytes) return false;
    WAVEFORMATEX guest_format{};
    if (!guest_read(address, &guest_format, sizeof(guest_format))) {
        set_error(std::string("DirectSound ") + operation +
                  " WAVEFORMATEX is not readable in guest memory");
        return false;
    }
    const size_t total_bytes = sizeof(WAVEFORMATEX) +
                               static_cast<size_t>(guest_format.cbSize);
    if (static_cast<uint64_t>(address) + total_bytes >
        static_cast<uint64_t>(UINT32_MAX) + 1u) {
        set_error(std::string("DirectSound ") + operation +
                  " WAVEFORMATEX range wraps the x86 guest address space");
        return false;
    }
    try {
        native_bytes->resize(total_bytes);
    } catch (...) {
        set_error(std::string("host allocation failed while marshalling DirectSound ") +
                  operation + " WAVEFORMATEX");
        return false;
    }
    if (!guest_read(address, native_bytes->data(),
                    static_cast<uint32_t>(native_bytes->size()))) {
        set_error(std::string("DirectSound ") + operation +
                  " WAVEFORMATEX or cbSize data is unmapped");
        return false;
    }
    return true;
}

int invoke_create_sound_buffer(GuestObject &object, const uint32_t *args,
                               uint32_t *result_out)
{
    GuestDsBufferDesc guest_desc{};
    if (!guest_read(args[1], &guest_desc, sizeof(guest_desc))) {
        set_error("DirectSound CreateSoundBuffer descriptor is not readable in guest memory");
        return AC1_DS8_ERROR;
    }
    if (guest_desc.size != sizeof(guest_desc)) {
        set_error("DirectSound CreateSoundBuffer descriptor does not use the observed 36-byte x86 ABI");
        return AC1_DS8_METHOD_UNSUPPORTED;
    }
    if (args[3] != 0u) {
        set_error("DirectSound CreateSoundBuffer aggregation is unsupported");
        return AC1_DS8_METHOD_UNSUPPORTED;
    }
    uint32_t *guest_output = static_cast<uint32_t *>(
        guest_memory(args[2], sizeof(uint32_t), AC1_DS8_GUEST_WRITE));
    if (!guest_output) {
        set_error("DirectSound CreateSoundBuffer output is not writable in guest memory");
        return AC1_DS8_ERROR;
    }

    DSBUFFERDESC native_desc{};
    native_desc.dwSize = sizeof(native_desc);
    native_desc.dwFlags = guest_desc.flags;
    native_desc.dwBufferBytes = guest_desc.buffer_bytes;
    native_desc.dwReserved = guest_desc.reserved;
    native_desc.lpwfxFormat = nullptr;
    native_desc.guid3DAlgorithm = guest_desc.algorithm;

    std::vector<uint8_t> native_format_bytes;
    if (guest_desc.wave_format) {
        if (!copy_guest_wave_format(guest_desc.wave_format, &native_format_bytes,
                                    "CreateSoundBuffer"))
            return AC1_DS8_ERROR;
        native_desc.lpwfxFormat =
            reinterpret_cast<LPWAVEFORMATEX>(native_format_bytes.data());
    }

    LPDIRECTSOUNDBUFFER native_buffer = nullptr;
    const HRESULT result = reinterpret_cast<IDirectSound8 *>(object.native)
        ->CreateSoundBuffer(&native_desc, &native_buffer, nullptr);
    if (FAILED(result)) {
        if (native_buffer) native_buffer->Release();
        *guest_output = 0;
        *result_out = static_cast<uint32_t>(result);
        return AC1_DS8_HANDLED;
    }
    if (!native_buffer) {
        set_error("DirectSound CreateSoundBuffer succeeded with a null buffer");
        return AC1_DS8_ERROR;
    }
    const uint32_t guest_buffer = wrap_native(native_buffer, ObjectKind::Buffer);
    if (!guest_buffer) return AC1_DS8_ERROR;
    *guest_output = guest_buffer;
    *result_out = static_cast<uint32_t>(result);
    return AC1_DS8_HANDLED;
}

int invoke_buffer_set_format(GuestObject &object, const uint32_t *args,
                             uint32_t *result_out)
{
    std::vector<uint8_t> native_bytes;
    if (!copy_guest_wave_format(args[1], &native_bytes, "SetFormat"))
        return AC1_DS8_ERROR;
    const LPCWAVEFORMATEX format =
        reinterpret_cast<LPCWAVEFORMATEX>(native_bytes.data());
    const HRESULT result = object.kind == ObjectKind::Buffer8
        ? reinterpret_cast<IDirectSoundBuffer8 *>(object.native)->SetFormat(format)
        : reinterpret_cast<IDirectSoundBuffer *>(object.native)->SetFormat(format);
    *result_out = static_cast<uint32_t>(result);
    return AC1_DS8_HANDLED;
}

int invoke_buffer_lock(GuestObject &object, const uint32_t *args,
                       uint32_t *result_out)
{
    if (object.lock.active) {
        set_error("DirectSound buffer Lock called again before its prior Unlock");
        return AC1_DS8_ERROR;
    }

    uint32_t *guest_pointer1_output = static_cast<uint32_t *>(
        guest_memory(args[3], sizeof(uint32_t), AC1_DS8_GUEST_WRITE));
    uint32_t *guest_bytes1_output = static_cast<uint32_t *>(
        guest_memory(args[4], sizeof(uint32_t), AC1_DS8_GUEST_WRITE));
    uint32_t *guest_pointer2_output = static_cast<uint32_t *>(
        guest_memory(args[5], sizeof(uint32_t), AC1_DS8_GUEST_WRITE));
    uint32_t *guest_bytes2_output = static_cast<uint32_t *>(
        guest_memory(args[6], sizeof(uint32_t), AC1_DS8_GUEST_WRITE));
    if (!guest_pointer1_output || !guest_bytes1_output ||
        !guest_pointer2_output || !guest_bytes2_output) {
        set_error("DirectSound Lock output parameters are not writable in guest memory");
        return AC1_DS8_ERROR;
    }
    *guest_pointer1_output = 0;
    *guest_bytes1_output = 0;
    *guest_pointer2_output = 0;
    *guest_bytes2_output = 0;

    void *host_pointer1 = nullptr;
    void *host_pointer2 = nullptr;
    DWORD host_bytes1 = 0;
    DWORD host_bytes2 = 0;
    const HRESULT result = native_buffer(object)->Lock(
        args[1], args[2], &host_pointer1, &host_bytes1,
        &host_pointer2, &host_bytes2, args[7]);
    if (FAILED(result)) {
        *result_out = static_cast<uint32_t>(result);
        return AC1_DS8_HANDLED;
    }

    object.lock.active = true;
    object.lock.host_pointer1 = host_pointer1;
    object.lock.host_bytes1 = host_bytes1;
    object.lock.host_pointer2 = host_pointer2;
    object.lock.host_bytes2 = host_bytes2;
    if ((host_bytes1 && !host_pointer1) || (host_bytes2 && !host_pointer2)) {
        set_error("native DirectSound Lock returned a null segment with nonzero size");
        (void)unlock_native_buffer(object);
        return AC1_DS8_ERROR;
    }

    if (host_bytes1) {
        object.lock.guest_pointer1 = g_callbacks.guest_alloc(
            g_context, host_bytes1, 16u);
        if (!object.lock.guest_pointer1) {
            set_error("guest allocation failed for DirectSound Lock segment 1");
            (void)unlock_native_buffer(object);
            return AC1_DS8_ERROR;
        }
        void *guest_segment = guest_memory(object.lock.guest_pointer1, host_bytes1,
                                           AC1_DS8_GUEST_WRITE);
        if (!guest_segment) {
            set_error("allocated DirectSound Lock segment 1 is not guest-writable");
            (void)unlock_native_buffer(object);
            return AC1_DS8_ERROR;
        }
        std::memcpy(guest_segment, host_pointer1, host_bytes1);
    }
    if (host_bytes2) {
        object.lock.guest_pointer2 = g_callbacks.guest_alloc(
            g_context, host_bytes2, 16u);
        if (!object.lock.guest_pointer2) {
            set_error("guest allocation failed for DirectSound Lock segment 2");
            (void)unlock_native_buffer(object);
            return AC1_DS8_ERROR;
        }
        void *guest_segment = guest_memory(object.lock.guest_pointer2, host_bytes2,
                                           AC1_DS8_GUEST_WRITE);
        if (!guest_segment) {
            set_error("allocated DirectSound Lock segment 2 is not guest-writable");
            (void)unlock_native_buffer(object);
            return AC1_DS8_ERROR;
        }
        std::memcpy(guest_segment, host_pointer2, host_bytes2);
    }

    const uint32_t guest_pointer1 = object.lock.guest_pointer1;
    const uint32_t guest_pointer2 = object.lock.guest_pointer2;
    if (!guest_write(args[3], &guest_pointer1, sizeof(guest_pointer1)) ||
        !guest_write(args[4], &host_bytes1, sizeof(host_bytes1)) ||
        !guest_write(args[5], &guest_pointer2, sizeof(guest_pointer2)) ||
        !guest_write(args[6], &host_bytes2, sizeof(host_bytes2))) {
        set_error("DirectSound Lock outputs became unwritable guest addresses");
        (void)unlock_native_buffer(object);
        return AC1_DS8_ERROR;
    }

    *result_out = static_cast<uint32_t>(result);
    return AC1_DS8_HANDLED;
}

int invoke_buffer_unlock(GuestObject &object, const uint32_t *args,
                         uint32_t *result_out)
{
    if (!object.lock.active || args[1] != object.lock.guest_pointer1 ||
        args[2] != object.lock.host_bytes1 ||
        args[3] != object.lock.guest_pointer2 ||
        args[4] != object.lock.host_bytes2) {
        set_error("DirectSound Unlock does not match an outstanding guest Lock");
        return AC1_DS8_ERROR;
    }

    if (object.lock.host_bytes1) {
        const void *guest_segment = guest_memory(object.lock.guest_pointer1,
                                                 object.lock.host_bytes1,
                                                 AC1_DS8_GUEST_READ);
        if (!guest_segment) {
            set_error("DirectSound Unlock segment 1 is not readable in guest memory");
            return AC1_DS8_ERROR;
        }
        std::memcpy(object.lock.host_pointer1, guest_segment,
                    object.lock.host_bytes1);
    }
    if (object.lock.host_bytes2) {
        const void *guest_segment = guest_memory(object.lock.guest_pointer2,
                                                 object.lock.host_bytes2,
                                                 AC1_DS8_GUEST_READ);
        if (!guest_segment) {
            set_error("DirectSound Unlock segment 2 is not readable in guest memory");
            return AC1_DS8_ERROR;
        }
        std::memcpy(object.lock.host_pointer2, guest_segment,
                    object.lock.host_bytes2);
    }

    const HRESULT result = native_buffer(object)->Unlock(
        object.lock.host_pointer1, object.lock.host_bytes1,
        object.lock.host_pointer2, object.lock.host_bytes2);
    if (SUCCEEDED(result)) {
        free_lock_shadows(object);
        object.lock = {};
    }
    *result_out = static_cast<uint32_t>(result);
    return AC1_DS8_HANDLED;
}

int invoke_release(uint32_t address, std::map<uint32_t, GuestObject>::iterator found,
                   uint32_t *result_out)
{
    GuestObject &object = found->second;
    if (is_buffer_kind(object.kind) && object.lock.active) {
        set_error("DirectSound buffer Release reached with an outstanding guest Lock");
        return AC1_DS8_ERROR;
    }
    if (!object.guest_references) {
        set_error("DirectSound proxy reference count is already zero");
        return AC1_DS8_ERROR;
    }
    --object.guest_references;
    const ULONG result = object.native->Release();
    *result_out = static_cast<uint32_t>(result);
    if (!object.guest_references) {
        free_guest_proxy(address, object);
        g_objects.erase(found);
    }
    return AC1_DS8_HANDLED;
}

} // namespace

extern "C" int ac1_ds8_configure(const ac1_ds8_callbacks *callbacks,
                                  void *context)
{
    if (!callbacks || callbacks->struct_size < sizeof(ac1_ds8_callbacks) ||
        !callbacks->guest_resolve || !callbacks->guest_alloc ||
        !callbacks->guest_free || !callbacks->guest_hwnd_to_host)
        return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_objects.empty()) {
        set_error("cannot replace DirectSound callbacks while guest COM proxies are live");
        return 0;
    }
    g_callbacks = *callbacks;
    g_context = context;
    clear_error();
    return 1;
}

extern "C" int ac1_ds8_create8(uint32_t guid_address, uint32_t output_address,
                                uint32_t outer_address, uint32_t *result_out)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    clear_error();
    if (!result_out || !g_callbacks.guest_resolve || !g_callbacks.guest_alloc ||
        !g_callbacks.guest_free) {
        set_error("DirectSoundCreate8 has no configured guest address space");
        return AC1_DS8_ERROR;
    }
    if (outer_address) {
        set_error("DirectSoundCreate8 aggregation is unsupported");
        return AC1_DS8_METHOD_UNSUPPORTED;
    }
    uint32_t *guest_output = static_cast<uint32_t *>(
        guest_memory(output_address, sizeof(uint32_t), AC1_DS8_GUEST_WRITE));
    if (!guest_output) {
        set_error("DirectSoundCreate8 output is not writable in guest memory");
        return AC1_DS8_ERROR;
    }

    GUID guest_guid{};
    const GUID *native_guid = nullptr;
    if (guid_address) {
        if (!guest_read(guid_address, &guest_guid, sizeof(guest_guid))) {
            set_error("DirectSoundCreate8 GUID is not readable in guest memory");
            return AC1_DS8_ERROR;
        }
        native_guid = &guest_guid;
    }

    DirectSoundCreate8Fn create = find_create8();
    if (!create) return AC1_DS8_ERROR;
    LPDIRECTSOUND8 native = nullptr;
    const HRESULT result = create(native_guid, &native, nullptr);
    if (FAILED(result)) {
        if (native) native->Release();
        *guest_output = 0;
        *result_out = static_cast<uint32_t>(result);
        return AC1_DS8_HANDLED;
    }
    if (!native) {
        set_error("DirectSoundCreate8 succeeded with a null device");
        return AC1_DS8_ERROR;
    }
    const uint32_t guest_object = wrap_native(native, ObjectKind::Device);
    if (!guest_object) return AC1_DS8_ERROR;
    *guest_output = guest_object;
    *result_out = static_cast<uint32_t>(result);
    return AC1_DS8_HANDLED;
}

extern "C" int ac1_ds8_argument_count(uint32_t token)
{
    return token_argument_count(token);
}

extern "C" int ac1_ds8_dispatch(uint32_t token, const uint32_t *args,
                                 uint32_t arg_count, uint32_t *result_out)
{
    const int expected_arguments = token_argument_count(token);
    if (expected_arguments == 0) return AC1_DS8_UNSUPPORTED;
    if (expected_arguments == -2) {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::ostringstream message;
        message << "DirectSound COM token 0x" << std::hex << token
                << " names an unsupported method/profile";
        set_error(message.str());
        return AC1_DS8_METHOD_UNSUPPORTED;
    }
    if (expected_arguments < 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        set_error("malformed DirectSound COM token or vtable slot");
        return AC1_DS8_ERROR;
    }
    if (!args || !result_out || arg_count != static_cast<uint32_t>(expected_arguments)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        set_error("invalid DirectSound COM dispatch arguments");
        return AC1_DS8_ERROR;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    clear_error();
    const uint32_t offset = token - AC1_DS8_TOKEN_BASE;
    const ObjectKind kind = static_cast<ObjectKind>(offset / kKindStride);
    const uint32_t method = (offset % kKindStride) / kSlotStride;
    auto found = g_objects.find(args[0]);
    if (found == g_objects.end() || found->second.kind != kind) {
        set_error("DirectSound COM token called with an unknown guest proxy/interface");
        return AC1_DS8_ERROR;
    }
    GuestObject &object = found->second;

    if (method == 0u) return invoke_query_interface(object, args, result_out);
    if (method == 1u) {
        if (!object.guest_references) {
            set_error("DirectSound proxy reference count is already zero");
            return AC1_DS8_ERROR;
        }
        ++object.guest_references;
        *result_out = static_cast<uint32_t>(object.native->AddRef());
        return AC1_DS8_HANDLED;
    }
    if (method == 2u) return invoke_release(args[0], found, result_out);

    if (kind == ObjectKind::Device && method == 3u)
        return invoke_create_sound_buffer(object, args, result_out);
    if (kind == ObjectKind::Device && method == 6u) {
        const uintptr_t window = g_callbacks.guest_hwnd_to_host(g_context, args[1]);
        if (!window) {
            set_error("DirectSound SetCooperativeLevel HWND is not mapped to a native window");
            return AC1_DS8_ERROR;
        }
        const HRESULT result = reinterpret_cast<IDirectSound8 *>(object.native)
            ->SetCooperativeLevel(reinterpret_cast<HWND>(window), args[2]);
        *result_out = static_cast<uint32_t>(result);
        return AC1_DS8_HANDLED;
    }
    if (kind == ObjectKind::Device && method == 8u) {
        uint32_t *guest_output = static_cast<uint32_t *>(
            guest_memory(args[1], sizeof(uint32_t), AC1_DS8_GUEST_WRITE));
        if (!guest_output) {
            set_error("DirectSound GetSpeakerConfig output is not writable in guest memory");
            return AC1_DS8_ERROR;
        }
        DWORD speaker_config = 0;
        const HRESULT result = reinterpret_cast<IDirectSound8 *>(object.native)
            ->GetSpeakerConfig(&speaker_config);
        if (SUCCEEDED(result)) *guest_output = speaker_config;
        *result_out = static_cast<uint32_t>(result);
        return AC1_DS8_HANDLED;
    }
    if (is_buffer_kind(kind) && method == 11u)
        return invoke_buffer_lock(object, args, result_out);
    if (is_buffer_kind(kind) && method == 12u) {
        const HRESULT result = native_buffer(object)->Play(args[1], args[2], args[3]);
        *result_out = static_cast<uint32_t>(result);
        return AC1_DS8_HANDLED;
    }
    if (is_buffer_kind(kind) && method == 14u)
        return invoke_buffer_set_format(object, args, result_out);
    if (is_buffer_kind(kind) && method == 15u) {
        const HRESULT result = native_buffer(object)->SetVolume(
            static_cast<LONG>(args[1]));
        *result_out = static_cast<uint32_t>(result);
        return AC1_DS8_HANDLED;
    }
    if (is_buffer_kind(kind) && method == 16u) {
        const HRESULT result = native_buffer(object)->SetPan(
            static_cast<LONG>(args[1]));
        *result_out = static_cast<uint32_t>(result);
        return AC1_DS8_HANDLED;
    }
    if (is_buffer_kind(kind) && method == 19u)
        return invoke_buffer_unlock(object, args, result_out);

    set_error("DirectSound method reached an unsupported implementation path");
    return AC1_DS8_METHOD_UNSUPPORTED;
}

extern "C" void ac1_ds8_shutdown(void)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (unsigned phase = 0; phase < 2u; ++phase) {
        for (auto &entry : g_objects) {
            GuestObject &object = entry.second;
            const bool is_device = object.kind == ObjectKind::Device;
            if ((phase == 0u && is_device) || (phase == 1u && !is_device)) continue;
            if (is_buffer_kind(object.kind) && object.lock.active) {
                const HRESULT unlock_result = unlock_native_buffer(object);
                if (FAILED(unlock_result))
                    set_error("native DirectSound Unlock failed while shutting down a guest-stop lock");
            }
            for (uint32_t refs = object.guest_references; refs; --refs)
                object.native->Release();
            if (is_buffer_kind(object.kind)) {
                free_lock_shadows(object);
                object.lock = {};
            }
            free_guest_proxy(entry.first, object);
        }
    }
    g_objects.clear();
    if (g_dsound_module) {
        FreeLibrary(g_dsound_module);
        g_dsound_module = nullptr;
    }
}

extern "C" const char *ac1_ds8_last_error(void)
{
    return g_last_error.c_str();
}
