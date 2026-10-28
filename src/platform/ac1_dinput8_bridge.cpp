#define DIRECTINPUT_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "ac1_dinput8_bridge.h"

#include <windows.h>
#include <dinput.h>

#include <cstring>
#include <cstdio>
#include <map>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kKindStride = 0x1000u;
constexpr uint32_t kSlotStride = 0x10u;

enum class ObjectKind : uint32_t {
    Unknown = 1,
    InputA = 2,
    InputW = 3,
    DeviceA = 4,
    DeviceW = 5
};

const GUID kIidUnknown =
    {0x00000000u, 0x0000u, 0x0000u, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
const GUID kIidInputA =
    {0xBF798030u, 0x483Au, 0x4DA2u, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
const GUID kIidInputW =
    {0xBF798031u, 0x483Au, 0x4DA2u, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
const GUID kIidDeviceA =
    {0x54D41080u, 0xDC15u, 0x4833u, {0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79}};
const GUID kIidDeviceW =
    {0x54D41081u, 0xDC15u, 0x4833u, {0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79}};

struct GuestObject {
    IUnknown *native = nullptr;
    ObjectKind kind = ObjectKind::Unknown;
    uint32_t guest_vtable = 0;
    uint32_t guest_references = 1;
};

struct PendingEnumBatch {
    std::vector<uint8_t> bytes;
    uint32_t record_count = 0u;
    uint32_t record_size = 0u;
    HRESULT native_hresult = E_FAIL;
};

#pragma pack(push, 4)
struct GuestDataFormat {
    uint32_t size;
    uint32_t object_size;
    uint32_t flags;
    uint32_t data_size;
    uint32_t object_count;
    uint32_t objects;
};

struct GuestObjectDataFormat {
    uint32_t guid;
    uint32_t offset;
    uint32_t type;
    uint32_t flags;
};
#pragma pack(pop)

ac1_dinput8_callbacks g_callbacks{};
void *g_context = nullptr;
std::map<uint32_t, GuestObject> g_objects;
std::mutex g_mutex;
thread_local char g_last_error[1024]{};
std::map<DWORD, PendingEnumBatch> g_pending_enum_batches;
HMODULE g_dinput_module = nullptr;

static_assert(sizeof(DIDEVICEOBJECTINSTANCEW) == 0x240u,
              "DirectInput 8.0 W object-instance ABI changed");
static_assert(sizeof(DIDEVICEINSTANCEW) == 0x44cu,
              "DirectInput 8.0 W device-instance ABI changed");
static_assert(sizeof(GuestDataFormat) == 0x18u,
              "guest x86 DIDATAFORMAT ABI must be 24 bytes");
static_assert(sizeof(GuestObjectDataFormat) == 0x10u,
              "guest x86 DIOBJECTDATAFORMAT ABI must be 16 bytes");

void set_error(const std::string &message)
{
    std::snprintf(g_last_error, sizeof(g_last_error), "%s", message.c_str());
    if (g_callbacks.log) g_callbacks.log(g_context, message.c_str());
    else OutputDebugStringA(("AC1 DirectInput8 bridge: " + message + "\n").c_str());
}

void clear_error() { g_last_error[0] = '\0'; }

void *guest_memory(uint32_t address, uint32_t size, uint32_t access)
{
    if (!address || !size || !g_callbacks.guest_resolve) return nullptr;
    return g_callbacks.guest_resolve(g_context, address, size, access);
}

bool guest_read(uint32_t address, void *output, uint32_t size)
{
    void *source = guest_memory(address, size, AC1_DINPUT8_GUEST_READ);
    if (!source) return false;
    std::memcpy(output, source, size);
    return true;
}

bool guest_write(uint32_t address, const void *input, uint32_t size)
{
    void *destination = guest_memory(address, size, AC1_DINPUT8_GUEST_WRITE);
    if (!destination) return false;
    std::memcpy(destination, input, size);
    return true;
}

uint32_t slot_count(ObjectKind kind)
{
    switch (kind) {
    case ObjectKind::Unknown: return 3u;
    case ObjectKind::InputA:
    case ObjectKind::InputW: return 11u;
    case ObjectKind::DeviceA:
    case ObjectKind::DeviceW: return 32u;
    }
    return 0u;
}

uint32_t slot_token(ObjectKind kind, uint32_t slot)
{
    return AC1_DINPUT8_TOKEN_BASE + static_cast<uint32_t>(kind) * kKindStride +
           slot * kSlotStride;
}

bool is_input(ObjectKind kind)
{
    return kind == ObjectKind::InputA || kind == ObjectKind::InputW;
}

bool is_device(ObjectKind kind)
{
    return kind == ObjectKind::DeviceA || kind == ObjectKind::DeviceW;
}

ObjectKind kind_for_iid(const GUID &iid)
{
    if (IsEqualGUID(iid, kIidUnknown)) return ObjectKind::Unknown;
    if (IsEqualGUID(iid, kIidInputA)) return ObjectKind::InputA;
    if (IsEqualGUID(iid, kIidInputW)) return ObjectKind::InputW;
    if (IsEqualGUID(iid, kIidDeviceA)) return ObjectKind::DeviceA;
    if (IsEqualGUID(iid, kIidDeviceW)) return ObjectKind::DeviceW;
    return static_cast<ObjectKind>(0u);
}

bool ensure_dinput_module()
{
    if (g_dinput_module) return true;
    wchar_t system_directory[32768]{};
    const UINT length = GetSystemDirectoryW(
        system_directory,
        static_cast<UINT>(sizeof(system_directory) / sizeof(system_directory[0])));
    static constexpr wchar_t suffix[] = L"\\dinput8.dll";
    const UINT capacity = static_cast<UINT>(sizeof(system_directory) /
                                            sizeof(system_directory[0]));
    if (!length) {
        set_error("GetSystemDirectoryW failed while locating System32 dinput8.dll");
        return false;
    }
    if (length >= capacity || length + (sizeof(suffix) / sizeof(suffix[0])) > capacity) {
        set_error("System32 path does not fit while loading dinput8.dll");
        return false;
    }
    std::memcpy(system_directory + length, suffix, sizeof(suffix));
    g_dinput_module = LoadLibraryW(system_directory);
    if (!g_dinput_module) {
        std::ostringstream message;
        message << "System32 dinput8.dll could not load (Win32 error "
                << GetLastError() << ')';
        set_error(message.str());
        return false;
    }
    return true;
}

using DirectInput8CreateFn = HRESULT (WINAPI *)(HINSTANCE, DWORD, REFIID,
                                                 LPVOID *, LPUNKNOWN);

DirectInput8CreateFn find_create8()
{
    if (!ensure_dinput_module()) return nullptr;
    FARPROC raw = GetProcAddress(g_dinput_module, "DirectInput8Create");
    if (!raw) {
        std::ostringstream message;
        message << "System32 DirectInput8Create is unavailable (Win32 error "
                << GetLastError() << ')';
        set_error(message.str());
        return nullptr;
    }
    static_assert(sizeof(DirectInput8CreateFn) == sizeof(FARPROC),
                  "Windows function-pointer ABI mismatch");
    DirectInput8CreateFn function = nullptr;
    std::memcpy(&function, &raw, sizeof(function));
    return function;
}

void free_guest_proxy(uint32_t address, const GuestObject &object)
{
    if (!g_callbacks.guest_free) return;
    if (object.guest_vtable) g_callbacks.guest_free(g_context, object.guest_vtable);
    if (address) g_callbacks.guest_free(g_context, address);
}

uint32_t wrap_native(IUnknown *native, ObjectKind kind)
{
    if (!native) return 0;
    const uint32_t slots = slot_count(kind);
    if (!slots || !g_callbacks.guest_alloc || !g_callbacks.guest_free) {
        set_error("cannot wrap an unknown DirectInput interface kind");
        native->Release();
        return 0;
    }
    const uint32_t table_bytes = slots * sizeof(uint32_t);
    const uint32_t table = g_callbacks.guest_alloc(g_context, table_bytes, 4u);
    if (!table) {
        set_error("guest allocation failed for DirectInput proxy vtable");
        native->Release();
        return 0;
    }
    const uint32_t address = g_callbacks.guest_alloc(g_context, sizeof(uint32_t), 4u);
    if (!address) {
        set_error("guest allocation failed for DirectInput proxy object");
        g_callbacks.guest_free(g_context, table);
        native->Release();
        return 0;
    }
    void *table_memory = guest_memory(table, table_bytes, AC1_DINPUT8_GUEST_WRITE);
    if (!table_memory) {
        set_error("allocated DirectInput vtable is not guest-writable");
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
        set_error("allocated DirectInput proxy object is not guest-writable");
        g_callbacks.guest_free(g_context, address);
        g_callbacks.guest_free(g_context, table);
        native->Release();
        return 0;
    }
    GuestObject object;
    object.native = native;
    object.kind = kind;
    object.guest_vtable = table;
    try {
        if (!g_objects.emplace(address, object).second) {
            set_error("guest allocator reused a live DirectInput proxy address");
            g_callbacks.guest_free(g_context, address);
            g_callbacks.guest_free(g_context, table);
            native->Release();
            return 0;
        }
    } catch (...) {
        set_error("host allocation failed while tracking DirectInput proxy");
        g_callbacks.guest_free(g_context, address);
        g_callbacks.guest_free(g_context, table);
        native->Release();
        return 0;
    }
    return address;
}

int method_argument_count(uint32_t token)
{
    if (token < AC1_DINPUT8_TOKEN_BASE + kKindStride ||
        token >= AC1_DINPUT8_TOKEN_BASE + 6u * kKindStride)
        return AC1_DINPUT8_NOT_HANDLED;
    const uint32_t offset = token - AC1_DINPUT8_TOKEN_BASE;
    const uint32_t kind_value = offset / kKindStride;
    const uint32_t within_kind = offset % kKindStride;
    if (kind_value < static_cast<uint32_t>(ObjectKind::Unknown) ||
        kind_value > static_cast<uint32_t>(ObjectKind::DeviceW) ||
        within_kind % kSlotStride)
        return AC1_DINPUT8_ERROR;
    const ObjectKind kind = static_cast<ObjectKind>(kind_value);
    const uint32_t slot = within_kind / kSlotStride;
    if (slot >= slot_count(kind)) return AC1_DINPUT8_ERROR;
    if (slot == 0u) return 3; /* QueryInterface(this, iid, output). */
    if (slot == 1u || slot == 2u) return 1; /* AddRef / Release. */
    if (is_input(kind) && slot == 3u)
        return 4; /* CreateDevice(this, guid, output, outer). */
    if (kind == ObjectKind::InputW && slot == 4u)
        return 5; /* EnumDevices(this, type, callback, ref, flags). */
    if (kind == ObjectKind::DeviceW && slot == 4u)
        return 4; /* EnumObjects(this, callback, ref, flags). */
    if (kind == ObjectKind::DeviceW && slot == 11u)
        return 2; /* SetDataFormat(this, guest DIDATAFORMAT*). */
    return AC1_DINPUT8_METHOD_UNSUPPORTED;
}

struct EnumObjectsCollector {
    PendingEnumBatch *batch = nullptr;
    bool failed = false;
};

template <typename T>
bool append_enum_record(PendingEnumBatch *batch, const T *record)
{
    if (!batch || !record || batch->record_size != sizeof(T) ||
        batch->record_count >= 4096u ||
        batch->bytes.size() + sizeof(T) > UINT32_MAX)
        return false;
    try {
        const auto *bytes = reinterpret_cast<const uint8_t *>(record);
        batch->bytes.insert(batch->bytes.end(), bytes, bytes + sizeof(T));
    } catch (...) {
        return false;
    }
    ++batch->record_count;
    return true;
}

BOOL CALLBACK collect_device_object_w(LPCDIDEVICEOBJECTINSTANCEW instance,
                                      LPVOID context)
{
    auto *collector = static_cast<EnumObjectsCollector *>(context);
    if (!collector || !collector->batch || !instance) {
        if (collector) collector->failed = true;
        return DIENUM_STOP;
    }
    if (!append_enum_record(collector->batch, instance)) {
        collector->failed = true;
        return DIENUM_STOP;
    }
    return DIENUM_CONTINUE;
}

int invoke_enum_objects(GuestObject &object, const uint32_t *args,
                        uint32_t *result_out)
{
    if (object.kind != ObjectKind::DeviceW) {
        set_error("DirectInput EnumObjects currently supports only the observed W interface");
        return AC1_DINPUT8_METHOD_UNSUPPORTED;
    }
    if (!args[1]) {
        set_error("DirectInput EnumObjects guest callback is null");
        return AC1_DINPUT8_ERROR;
    }
    const DWORD thread_id = GetCurrentThreadId();
    auto pending = g_pending_enum_batches.find(thread_id);
    if (pending != g_pending_enum_batches.end()) {
        set_error("previous DirectInput EnumObjects batch was not consumed by runtime");
        return AC1_DINPUT8_ERROR;
    }
    try {
        pending = g_pending_enum_batches.emplace(thread_id, PendingEnumBatch{}).first;
    } catch (...) {
        set_error("host allocation failed while starting EnumObjects collection");
        return AC1_DINPUT8_ERROR;
    }
    PendingEnumBatch &batch = pending->second;
    batch.record_size = sizeof(DIDEVICEOBJECTINSTANCEW);
    EnumObjectsCollector collector{&batch, false};
    const HRESULT result = reinterpret_cast<IDirectInputDevice8W *>(object.native)
        ->EnumObjects(collect_device_object_w, &collector, args[3]);
    batch.native_hresult = result;
    if (collector.failed) {
        g_pending_enum_batches.erase(pending);
        set_error("host allocation or native record failure while collecting EnumObjects");
        return AC1_DINPUT8_ERROR;
    }
    if (FAILED(result)) {
        batch.bytes.clear();
        batch.record_count = 0u;
    }
    *result_out = static_cast<uint32_t>(result);
    return AC1_DINPUT8_HANDLED;
}

struct EnumDevicesCollector {
    PendingEnumBatch *batch = nullptr;
    bool failed = false;
};

BOOL CALLBACK collect_device_instance_w(LPCDIDEVICEINSTANCEW instance,
                                        LPVOID context)
{
    auto *collector = static_cast<EnumDevicesCollector *>(context);
    if (!collector || !collector->batch || !instance) {
        if (collector) collector->failed = true;
        return DIENUM_STOP;
    }
    if (!append_enum_record(collector->batch, instance)) {
        collector->failed = true;
        return DIENUM_STOP;
    }
    return DIENUM_CONTINUE;
}

int invoke_enum_devices(GuestObject &object, const uint32_t *args,
                        uint32_t *result_out)
{
    if (object.kind != ObjectKind::InputW) {
        set_error("DirectInput EnumDevices currently supports only the observed W interface");
        return AC1_DINPUT8_METHOD_UNSUPPORTED;
    }
    if (!args[2]) {
        set_error("DirectInput EnumDevices guest callback is null");
        return AC1_DINPUT8_ERROR;
    }
    const DWORD thread_id = GetCurrentThreadId();
    auto pending = g_pending_enum_batches.find(thread_id);
    if (pending != g_pending_enum_batches.end()) {
        set_error("previous DirectInput enumeration batch was not consumed by runtime");
        return AC1_DINPUT8_ERROR;
    }
    try {
        pending = g_pending_enum_batches.emplace(thread_id, PendingEnumBatch{}).first;
    } catch (...) {
        set_error("host allocation failed while starting EnumDevices collection");
        return AC1_DINPUT8_ERROR;
    }
    PendingEnumBatch &batch = pending->second;
    batch.record_size = sizeof(DIDEVICEINSTANCEW);
    EnumDevicesCollector collector{&batch, false};
    const HRESULT result = reinterpret_cast<IDirectInput8W *>(object.native)
        ->EnumDevices(args[1], collect_device_instance_w, &collector, args[4]);
    batch.native_hresult = result;
    if (collector.failed) {
        g_pending_enum_batches.erase(pending);
        set_error("host allocation or native record failure while collecting EnumDevices");
        return AC1_DINPUT8_ERROR;
    }
    if (FAILED(result)) {
        batch.bytes.clear();
        batch.record_count = 0u;
    }
    *result_out = static_cast<uint32_t>(result);
    return AC1_DINPUT8_HANDLED;
}

int invoke_set_data_format(GuestObject &object, const uint32_t *args,
                           uint32_t *result_out)
{
    if (object.kind != ObjectKind::DeviceW) {
        set_error("DirectInput SetDataFormat currently supports only the observed W interface");
        return AC1_DINPUT8_METHOD_UNSUPPORTED;
    }
    GuestDataFormat guest_format{};
    if (!guest_read(args[1], &guest_format, sizeof(guest_format))) {
        set_error("DirectInput SetDataFormat descriptor is unreadable in guest memory");
        return AC1_DINPUT8_ERROR;
    }
    if (guest_format.size != sizeof(GuestDataFormat) ||
        guest_format.object_size != sizeof(GuestObjectDataFormat)) {
        set_error("DirectInput SetDataFormat descriptor does not use the observed 24/16-byte x86 ABI");
        return AC1_DINPUT8_METHOD_UNSUPPORTED;
    }
    if (guest_format.object_count > 4096u) {
        set_error("DirectInput SetDataFormat object count exceeds the supported ABI bound");
        return AC1_DINPUT8_METHOD_UNSUPPORTED;
    }

    std::vector<DIOBJECTDATAFORMAT> native_objects;
    std::vector<GUID> native_guids;
    try {
        native_objects.resize(guest_format.object_count);
        native_guids.resize(guest_format.object_count);
    } catch (...) {
        set_error("host allocation failed while marshalling DirectInput SetDataFormat objects");
        return AC1_DINPUT8_ERROR;
    }
    if (guest_format.object_count) {
        const uint64_t array_bytes64 =
            static_cast<uint64_t>(guest_format.object_count) * sizeof(GuestObjectDataFormat);
        if (!guest_format.objects || array_bytes64 > UINT32_MAX) {
            set_error("DirectInput SetDataFormat object array address or size is invalid");
            return AC1_DINPUT8_ERROR;
        }
        std::vector<GuestObjectDataFormat> guest_objects;
        try {
            guest_objects.resize(guest_format.object_count);
        } catch (...) {
            set_error("host allocation failed while reading DirectInput SetDataFormat objects");
            return AC1_DINPUT8_ERROR;
        }
        if (!guest_read(guest_format.objects, guest_objects.data(),
                        static_cast<uint32_t>(array_bytes64))) {
            set_error("DirectInput SetDataFormat object array is unreadable in guest memory");
            return AC1_DINPUT8_ERROR;
        }
        for (uint32_t i = 0; i < guest_format.object_count; ++i) {
            const GuestObjectDataFormat &guest_object = guest_objects[i];
            DIOBJECTDATAFORMAT &native_object = native_objects[i];
            if (guest_object.guid) {
                if (!guest_read(guest_object.guid, &native_guids[i], sizeof(GUID))) {
                    set_error("DirectInput SetDataFormat object GUID is unreadable in guest memory");
                    return AC1_DINPUT8_ERROR;
                }
                native_object.pguid = &native_guids[i];
            } else {
                native_object.pguid = nullptr;
            }
            native_object.dwOfs = guest_object.offset;
            native_object.dwType = guest_object.type;
            native_object.dwFlags = guest_object.flags;
        }
    }

    DIDATAFORMAT native_format{};
    native_format.dwSize = sizeof(native_format);
    native_format.dwObjSize = sizeof(DIOBJECTDATAFORMAT);
    native_format.dwFlags = guest_format.flags;
    native_format.dwDataSize = guest_format.data_size;
    native_format.dwNumObjs = guest_format.object_count;
    native_format.rgodf = native_objects.empty() ? nullptr : native_objects.data();
    const HRESULT result = reinterpret_cast<IDirectInputDevice8W *>(object.native)
        ->SetDataFormat(&native_format);
    *result_out = static_cast<uint32_t>(result);
    return AC1_DINPUT8_HANDLED;
}

int invoke_query_interface(GuestObject &object, const uint32_t *args,
                           uint32_t *result_out)
{
    GUID iid{};
    if (!guest_read(args[1], &iid, sizeof(iid))) {
        set_error("DirectInput QueryInterface IID is unreadable in guest memory");
        return AC1_DINPUT8_ERROR;
    }
    const ObjectKind result_kind = kind_for_iid(iid);
    if (result_kind == static_cast<ObjectKind>(0u) ||
        (is_input(object.kind) && is_device(result_kind)) ||
        (is_device(object.kind) && is_input(result_kind))) {
        set_error("DirectInput QueryInterface requested an unsupported proxy interface");
        return AC1_DINPUT8_METHOD_UNSUPPORTED;
    }
    uint32_t *output = static_cast<uint32_t *>(
        guest_memory(args[2], sizeof(uint32_t), AC1_DINPUT8_GUEST_WRITE));
    if (!output) {
        set_error("DirectInput QueryInterface output is not writable guest memory");
        return AC1_DINPUT8_ERROR;
    }
    *output = 0u;
    void *native_result = nullptr;
    const HRESULT result = object.native->QueryInterface(iid, &native_result);
    if (FAILED(result)) {
        if (native_result) static_cast<IUnknown *>(native_result)->Release();
        *result_out = static_cast<uint32_t>(result);
        return AC1_DINPUT8_HANDLED;
    }
    if (!native_result) {
        set_error("DirectInput QueryInterface succeeded with a null interface");
        return AC1_DINPUT8_ERROR;
    }
    const uint32_t guest_object = wrap_native(
        static_cast<IUnknown *>(native_result), result_kind);
    if (!guest_object) return AC1_DINPUT8_ERROR;
    *output = guest_object;
    *result_out = static_cast<uint32_t>(result);
    return AC1_DINPUT8_HANDLED;
}

int invoke_create_device(GuestObject &object, const uint32_t *args,
                         uint32_t *result_out)
{
    if (args[3] != 0u) {
        set_error("DirectInput CreateDevice aggregation is unsupported");
        return AC1_DINPUT8_METHOD_UNSUPPORTED;
    }
    uint32_t *output = static_cast<uint32_t *>(
        guest_memory(args[2], sizeof(uint32_t), AC1_DINPUT8_GUEST_WRITE));
    if (!output) {
        set_error("DirectInput CreateDevice output is not writable guest memory");
        return AC1_DINPUT8_ERROR;
    }
    GUID device_guid{};
    if (!guest_read(args[1], &device_guid, sizeof(device_guid))) {
        set_error("DirectInput CreateDevice GUID is unreadable in guest memory");
        return AC1_DINPUT8_ERROR;
    }
    *output = 0u;
    HRESULT result = E_NOINTERFACE;
    IUnknown *native = nullptr;
    if (object.kind == ObjectKind::InputA) {
        IDirectInputDevice8A *created = nullptr;
        result = reinterpret_cast<IDirectInput8A *>(object.native)->CreateDevice(
            device_guid, &created, nullptr);
        native = created;
    } else {
        IDirectInputDevice8W *created = nullptr;
        result = reinterpret_cast<IDirectInput8W *>(object.native)->CreateDevice(
            device_guid, &created, nullptr);
        native = created;
    }
    if (FAILED(result)) {
        if (native) native->Release();
        *result_out = static_cast<uint32_t>(result);
        return AC1_DINPUT8_HANDLED;
    }
    if (!native) {
        set_error("DirectInput CreateDevice succeeded with a null device interface");
        return AC1_DINPUT8_ERROR;
    }
    const ObjectKind device_kind = object.kind == ObjectKind::InputA
        ? ObjectKind::DeviceA : ObjectKind::DeviceW;
    const uint32_t guest_object = wrap_native(native, device_kind);
    if (!guest_object) return AC1_DINPUT8_ERROR;
    *output = guest_object;
    *result_out = static_cast<uint32_t>(result);
    return AC1_DINPUT8_HANDLED;
}

} // namespace

extern "C" int ac1_dinput8_configure(const ac1_dinput8_callbacks *callbacks,
                                      void *context)
{
    if (!callbacks || callbacks->struct_size < sizeof(ac1_dinput8_callbacks) ||
        !callbacks->guest_resolve || !callbacks->guest_alloc ||
        !callbacks->guest_free || !callbacks->guest_hwnd_to_host ||
        !callbacks->guest_hinstance_to_host)
        return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_objects.empty()) {
        set_error("cannot replace DirectInput callbacks while guest proxies are live");
        return 0;
    }
    g_callbacks = *callbacks;
    g_context = context;
    clear_error();
    return 1;
}

extern "C" int ac1_dinput8_create8(uint32_t guest_hinstance, uint32_t version,
                                    uint32_t guest_iid, uint32_t guest_output,
                                    uint32_t guest_outer,
                                    uint32_t *native_result_out)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    clear_error();
    if (!native_result_out || !g_callbacks.guest_resolve ||
        !g_callbacks.guest_alloc || !g_callbacks.guest_free ||
        !g_callbacks.guest_hinstance_to_host) {
        set_error("DirectInput8Create has no configured guest address space");
        return AC1_DINPUT8_ERROR;
    }
    if (guest_outer) {
        set_error("DirectInput8Create aggregation is unsupported");
        return AC1_DINPUT8_METHOD_UNSUPPORTED;
    }
    uint32_t *output = static_cast<uint32_t *>(
        guest_memory(guest_output, sizeof(uint32_t), AC1_DINPUT8_GUEST_WRITE));
    if (!output) {
        set_error("DirectInput8Create output is not writable guest memory");
        return AC1_DINPUT8_ERROR;
    }
    *output = 0u;
    GUID iid{};
    if (!guest_read(guest_iid, &iid, sizeof(iid))) {
        set_error("DirectInput8Create IID is unreadable in guest memory");
        return AC1_DINPUT8_ERROR;
    }
    const ObjectKind kind = kind_for_iid(iid);
    if (kind != ObjectKind::InputA && kind != ObjectKind::InputW) {
        set_error("DirectInput8Create requested an unsupported interface IID");
        return AC1_DINPUT8_METHOD_UNSUPPORTED;
    }
    const uintptr_t host_instance =
        g_callbacks.guest_hinstance_to_host(g_context, guest_hinstance);
    if (!host_instance) {
        set_error("guest HINSTANCE was not mapped to a native module handle");
        return AC1_DINPUT8_ERROR;
    }
    DirectInput8CreateFn create = find_create8();
    if (!create) return AC1_DINPUT8_ERROR;
    void *native_result = nullptr;
    const HRESULT result = create(reinterpret_cast<HINSTANCE>(host_instance),
                                  version, iid, &native_result, nullptr);
    if (FAILED(result)) {
        if (native_result) static_cast<IUnknown *>(native_result)->Release();
        *native_result_out = static_cast<uint32_t>(result);
        return AC1_DINPUT8_HANDLED;
    }
    if (!native_result) {
        set_error("DirectInput8Create succeeded with a null interface");
        return AC1_DINPUT8_ERROR;
    }
    const uint32_t guest_object = wrap_native(
        static_cast<IUnknown *>(native_result), kind);
    if (!guest_object) return AC1_DINPUT8_ERROR;
    *output = guest_object;
    *native_result_out = static_cast<uint32_t>(result);
    return AC1_DINPUT8_HANDLED;
}

extern "C" int ac1_dinput8_argument_count(uint32_t token)
{
    return method_argument_count(token);
}

extern "C" int ac1_dinput8_dispatch(uint32_t token, const uint32_t *args,
                                     uint32_t arg_count,
                                     uint32_t *result_out)
{
    const int expected = method_argument_count(token);
    if (expected == AC1_DINPUT8_NOT_HANDLED) return AC1_DINPUT8_NOT_HANDLED;
    if (expected == AC1_DINPUT8_METHOD_UNSUPPORTED) {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::ostringstream message;
        message << "DirectInput COM token 0x" << std::hex << token
                << " names an unsupported method/profile";
        set_error(message.str());
        return AC1_DINPUT8_METHOD_UNSUPPORTED;
    }
    if (expected < 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        set_error("malformed DirectInput COM token or vtable slot");
        return AC1_DINPUT8_ERROR;
    }
    if (!args || !result_out || arg_count != static_cast<uint32_t>(expected)) {
        std::lock_guard<std::mutex> lock(g_mutex);
        set_error("invalid DirectInput COM dispatch arguments");
        return AC1_DINPUT8_ERROR;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    clear_error();
    const uint32_t offset = token - AC1_DINPUT8_TOKEN_BASE;
    const ObjectKind kind = static_cast<ObjectKind>(offset / kKindStride);
    const uint32_t slot = (offset % kKindStride) / kSlotStride;
    auto found = g_objects.find(args[0]);
    if (found == g_objects.end() || found->second.kind != kind) {
        set_error("DirectInput COM token called with an unknown guest proxy/interface");
        return AC1_DINPUT8_ERROR;
    }
    GuestObject &object = found->second;
    if (slot == 0u) return invoke_query_interface(object, args, result_out);
    if (slot == 1u) {
        if (!object.guest_references) {
            set_error("DirectInput proxy reference count is already zero");
            return AC1_DINPUT8_ERROR;
        }
        ++object.guest_references;
        *result_out = static_cast<uint32_t>(object.native->AddRef());
        return AC1_DINPUT8_HANDLED;
    }
    if (slot == 2u) {
        if (!object.guest_references) {
            set_error("DirectInput proxy reference count is already zero");
            return AC1_DINPUT8_ERROR;
        }
        --object.guest_references;
        const ULONG result = object.native->Release();
        *result_out = static_cast<uint32_t>(result);
        if (!object.guest_references) {
            free_guest_proxy(args[0], object);
            g_objects.erase(found);
        }
        return AC1_DINPUT8_HANDLED;
    }
    if (is_input(kind) && slot == 3u)
        return invoke_create_device(object, args, result_out);
    if (kind == ObjectKind::InputW && slot == 4u)
        return invoke_enum_devices(object, args, result_out);
    if (kind == ObjectKind::DeviceW && slot == 4u)
        return invoke_enum_objects(object, args, result_out);
    if (kind == ObjectKind::DeviceW && slot == 11u)
        return invoke_set_data_format(object, args, result_out);
    set_error("DirectInput method reached an unsupported implementation path");
    return AC1_DINPUT8_METHOD_UNSUPPORTED;
}

extern "C" int ac1_dinput8_enum_batch_take(ac1_dinput8_enum_batch *batch)
{
    if (!batch) return AC1_DINPUT8_ERROR;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto pending = g_pending_enum_batches.find(GetCurrentThreadId());
    if (pending == g_pending_enum_batches.end()) {
        *batch = {};
        return AC1_DINPUT8_NOT_HANDLED;
    }
    auto *records = new (std::nothrow) std::vector<uint8_t>();
    if (!records) {
        set_error("host allocation failed while transferring EnumObjects records");
        return AC1_DINPUT8_ERROR;
    }
    records->swap(pending->second.bytes);
    batch->opaque = records;
    batch->count = pending->second.record_count;
    batch->record_size = pending->second.record_size;
    batch->native_hresult = static_cast<int32_t>(pending->second.native_hresult);
    g_pending_enum_batches.erase(pending);
    return AC1_DINPUT8_HANDLED;
}

extern "C" const void *ac1_dinput8_enum_batch_record(
    const ac1_dinput8_enum_batch *batch, uint32_t index)
{
    if (!batch || !batch->opaque || index >= batch->count || !batch->record_size)
        return nullptr;
    const auto *bytes = static_cast<const std::vector<uint8_t> *>(batch->opaque);
    const uint64_t offset = static_cast<uint64_t>(index) * batch->record_size;
    if (offset + batch->record_size > bytes->size()) return nullptr;
    return bytes->data() + static_cast<size_t>(offset);
}

extern "C" void ac1_dinput8_enum_batch_free(ac1_dinput8_enum_batch *batch)
{
    if (!batch) return;
    delete static_cast<std::vector<uint8_t> *>(batch->opaque);
    *batch = {};
}

extern "C" void ac1_dinput8_shutdown(void)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto &entry : g_objects) {
        GuestObject &object = entry.second;
        for (uint32_t refs = object.guest_references; refs; --refs)
            object.native->Release();
        free_guest_proxy(entry.first, object);
    }
    g_objects.clear();
    g_pending_enum_batches.clear();
    if (g_dinput_module) {
        FreeLibrary(g_dinput_module);
        g_dinput_module = nullptr;
    }
}

extern "C" const char *ac1_dinput8_last_error(void)
{
    return g_last_error;
}
