#include "ac1_network.h"
#include "guest_memory.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    GUEST_BASE = 0x00400000u,
    GUEST_DATA = GUEST_BASE + 0x100u,
    GUEST_MEMORY_SIZE = 0x400u,
    WSAEFAULT_VALUE = 10014u,
    WSANOTINITIALISED_VALUE = 10093u,
    WSAVERNOTSUPPORTED_VALUE = 10092u
};

#pragma pack(push, 4)
typedef struct guest_wsadata_layout {
    uint16_t version;
    uint16_t high_version;
    char description[257];
    char status[129];
    uint16_t max_sockets;
    uint16_t max_udp_datagram;
    uint32_t vendor_pointer;
} guest_wsadata_layout;
#pragma pack(pop)

_Static_assert(offsetof(guest_wsadata_layout, description) == 4u,
               "x86 WSADATA description offset");
_Static_assert(offsetof(guest_wsadata_layout, status) == 261u,
               "x86 WSADATA status offset");
_Static_assert(offsetof(guest_wsadata_layout, max_sockets) == 390u,
               "x86 WSADATA max-socket offset");
_Static_assert(offsetof(guest_wsadata_layout, max_udp_datagram) == 392u,
               "x86 WSADATA datagram offset");
_Static_assert(offsetof(guest_wsadata_layout, vendor_pointer) == 396u,
               "x86 WSADATA pointer alignment/padding");
_Static_assert(sizeof(guest_wsadata_layout) == 400u,
               "x86 WSADATA size");

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static int import_call(const char *name, uint16_t ordinal, int by_ordinal,
                       const uint32_t *arguments, uint32_t *result,
                       uint32_t *stack_bytes)
{
    return ac1_network_import("Ws2_32.DlL", name, ordinal, by_ordinal,
                              arguments, result, stack_bytes);
}

static int fail(const char *message)
{
    fprintf(stderr, "network compatibility check failed: %s\n", message);
    return 1;
}

int main(void)
{
    uint8_t guest_memory[GUEST_MEMORY_SIZE];
    memset(guest_memory, 0xa5, sizeof(guest_memory));
    guest_memory_bind(guest_memory, GUEST_BASE, sizeof(guest_memory));

    const uint32_t startup_args[2] = {0x0101u, GUEST_DATA};
    uint32_t result = UINT32_MAX, stack_bytes = UINT32_MAX;
    if (!import_call(NULL, 115u, 1, startup_args, &result, &stack_bytes))
        return fail("WSAStartup ordinal was not handled");
    if (stack_bytes != 8u) return fail("WSAStartup stdcall size");
    if (result != 0u) {
        fprintf(stderr, "WSAStartup 1.1 returned 0x%08x\n", result);
        return fail("expected available host/POSIX Winsock 1.1");
    }

    const uint8_t *data = guest_memory + (GUEST_DATA - GUEST_BASE);
    if (read_u16(data) != 0x0101u)
        return fail("WSADATA negotiated version offset/value");
    if (read_u16(data + 2u) < 0x0101u)
        return fail("WSADATA high-version offset/value");
    if (!memchr(data + 4u, 0, 257u))
        return fail("WSADATA description is not terminated in its x86 field");
    if (!memchr(data + 261u, 0, 129u))
        return fail("WSADATA status is not terminated in its x86 field");
#ifndef _WIN32
    if (read_u16(data + 390u) != 0u || read_u16(data + 392u) != 0u)
        return fail("POSIX provider unexpectedly reported socket limits");
#endif
    if (data[394] || data[395])
        return fail("WSADATA pointer alignment padding at bytes 394/395");
    if (data[396] || data[397] || data[398] || data[399])
        return fail("host vendor pointer escaped into guest address space");

    const uint32_t startup_args_again[2] = {0x0101u, GUEST_DATA};
    if (!import_call("WSAStartup", 0u, 0, startup_args_again,
                     &result, &stack_bytes) || result != 0u || stack_bytes != 8u)
        return fail("named WSAStartup second reference");

    const uint32_t asymmetric_version_args[2] = {0x0102u, GUEST_DATA};
    if (!import_call("WSAStartup", 0u, 0, asymmetric_version_args,
                     &result, &stack_bytes) || result != 0u || stack_bytes != 8u)
        return fail("MAKEWORD version 2.1 negotiation");
    data = guest_memory + (GUEST_DATA - GUEST_BASE);
    if (read_u16(data) != 0x0102u || read_u16(data + 2u) != 0x0202u)
        return fail("asymmetric version byte order/highest version");
    if (!import_call(NULL, 116u, 1, NULL, &result, &stack_bytes) || result != 0u)
        return fail("WSACleanup after asymmetric version test");

    const uint32_t bad_version_args[2] = {0x0100u, GUEST_DATA};
    if (!import_call("WSAStartup", 0u, 0, bad_version_args,
                     &result, &stack_bytes) ||
        result != WSAVERNOTSUPPORTED_VALUE || stack_bytes != 8u)
    {
        fprintf(stderr, "unsupported WSA version returned eax=0x%08x bytes=%u\n",
                result, stack_bytes);
        return fail("unsupported version result/reference handling");
    }

    const uint32_t bad_buffer_args[2] = {0x0101u, GUEST_BASE + GUEST_MEMORY_SIZE - 16u};
    if (!import_call("WSAStartup", 0u, 0, bad_buffer_args,
                     &result, &stack_bytes) ||
        result != WSAEFAULT_VALUE || stack_bytes != 8u)
        return fail("invalid guest WSADATA buffer result");

    const uint32_t set_error_args[1] = {4321u};
    if (!import_call("WSASetLastError", 0u, 0, set_error_args,
                     &result, &stack_bytes) || result != 0u || stack_bytes != 4u)
        return fail("named WSASetLastError");
    if (!import_call(NULL, 111u, 1, NULL, &result, &stack_bytes) ||
        result != 4321u || stack_bytes != 0u)
        return fail("WSAGetLastError thread state");

    if (!import_call(NULL, 116u, 1, NULL, &result, &stack_bytes) ||
        result != 0u || stack_bytes != 0u)
        return fail("first WSACleanup reference");
    if (!import_call("WSACleanup", 0u, 0, NULL, &result, &stack_bytes) ||
        result != 0u || stack_bytes != 0u)
        return fail("second WSACleanup reference");
    if (!import_call(NULL, 116u, 1, NULL, &result, &stack_bytes) ||
        result != UINT32_MAX || stack_bytes != 0u)
        return fail("unbalanced WSACleanup failure result");
    if (!import_call("WSAGetLastError", 0u, 0, NULL, &result, &stack_bytes) ||
        result != WSANOTINITIALISED_VALUE)
        return fail("WSACleanup WSAGetLastError state");

    ac1_network_reset();
    guest_memory_unbind();
    puts("Winsock 1.1 startup, x86 WSADATA layout, error state, and refcount passed");
    return 0;
}
