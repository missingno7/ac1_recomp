#include "ac1_d3d9_bridge.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
constexpr uint32_t kGuestBase = 0x00010000u;
constexpr uint32_t kGuestLimit = 0x00050000u;
constexpr uint32_t kGuestObjectPool = 0x00020000u;
std::array<uint8_t, kGuestLimit - kGuestBase> guest_memory{};
uint32_t next_guest_address = kGuestObjectPool;

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
        ac1_d3d9_argument_count(0x7e003000u) != 0 ||
        ac1_d3d9_dispatch(0x7e002030u, nullptr, 0, nullptr) != -2 ||
        ac1_d3d9_dispatch(0x7e003000u, nullptr, 0, nullptr) != 0) {
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
    if (ac1_d3d9_dispatch(release_token, this_args, 1, &remaining) != 1 || remaining != 0) {
        std::fprintf(stderr, "final Release dispatch failed: %s\n", ac1_d3d9_last_error());
        ac1_d3d9_shutdown();
        return 7;
    }
    ac1_d3d9_shutdown();
    std::printf("native Direct3DCreate9 and GetAdapterCount bridge smoke passed; adapters=%u\n",
                adapter_count);
    return 0;
}
