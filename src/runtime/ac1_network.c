#include "ac1_network.h"
#include "guest_memory.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#else
#define AC1_WSAEFAULT 10014u
#define AC1_WSAEINVAL 10022u
#define AC1_WSAENOTINITIALISED 10093u
#define AC1_WSAVERNOTSUPPORTED 10092u
#endif

enum {
    AC1_GUEST_WSADATA_SIZE = 400,
    AC1_GUEST_DESCRIPTION_OFFSET = 4,
    AC1_GUEST_DESCRIPTION_SIZE = 257,
    AC1_GUEST_STATUS_OFFSET = 261,
    AC1_GUEST_STATUS_SIZE = 129,
    AC1_GUEST_MAX_SOCKETS_OFFSET = 390,
    AC1_GUEST_MAX_UDP_OFFSET = 392,
    AC1_GUEST_VENDOR_POINTER_OFFSET = 396
};

#ifndef AC1_WSAENOTINITIALISED
#define AC1_WSAENOTINITIALISED 10093u
#endif

static _Thread_local uint32_t g_wsa_error;
static uint32_t g_startup_references;

static int same_ascii_fold(const char *left, const char *right)
{
    if (!left || !right) return 0;
    while (*left && *right) {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return 0;
    }
    return *left == *right;
}

static void put_u16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
}

static void copy_terminated(char *destination, size_t capacity,
                            const char *source, size_t source_capacity)
{
    size_t length = 0;
    if (!capacity) return;
    while (length + 1u < capacity && length < source_capacity &&
           source[length])
        ++length;
    if (length) memcpy(destination, source, length);
    destination[length] = '\0';
}

#ifndef _WIN32
static int version_less_equal(uint16_t left, uint16_t right)
{
    uint8_t left_major = (uint8_t)left;
    uint8_t right_major = (uint8_t)right;
    uint8_t left_minor = (uint8_t)(left >> 8);
    uint8_t right_minor = (uint8_t)(right >> 8);
    return left_major < right_major ||
           (left_major == right_major && left_minor <= right_minor);
}
#endif

#ifdef _WIN32
static void marshal_windows_wsadata(uint8_t *guest, const WSADATA *host)
{
    memset(guest, 0, AC1_GUEST_WSADATA_SIZE);
    put_u16(guest, host->wVersion);
    put_u16(guest + 2u, host->wHighVersion);
    copy_terminated((char *)guest + AC1_GUEST_DESCRIPTION_OFFSET,
                    AC1_GUEST_DESCRIPTION_SIZE, host->szDescription,
                    sizeof(host->szDescription));
    copy_terminated((char *)guest + AC1_GUEST_STATUS_OFFSET,
                    AC1_GUEST_STATUS_SIZE, host->szSystemStatus,
                    sizeof(host->szSystemStatus));
    put_u16(guest + AC1_GUEST_MAX_SOCKETS_OFFSET, host->iMaxSockets);
    put_u16(guest + AC1_GUEST_MAX_UDP_OFFSET, host->iMaxUdpDg);
    /* lpVendorInfo is a host pointer in native WSADATA. It cannot be exposed
     * as a guest address, so the guest structure advertises no vendor string. */
    memset(guest + AC1_GUEST_VENDOR_POINTER_OFFSET, 0, 4u);
}
#endif

static uint32_t network_startup(uint16_t requested_version,
                               uint32_t guest_wsadata)
{
    uint8_t *guest = guest_wsadata
        ? (uint8_t *)guest_memory_try_ptr(guest_wsadata, AC1_GUEST_WSADATA_SIZE)
        : NULL;
    if (!guest) return 10014u; /* WSAEFAULT: no guest WSADATA buffer. */

#ifdef _WIN32
    WSADATA host_data;
    memset(&host_data, 0, sizeof(host_data));
    int result = WSAStartup(requested_version, &host_data);
    if (result != 0) {
        /* WSAStartup returns its own error code. Per Winsock, callers must not
         * consult WSAGetLastError for this result. */
        return (uint32_t)result;
    }
    marshal_windows_wsadata(guest, &host_data);
    if (g_startup_references != UINT32_MAX) ++g_startup_references;
    return 0u;
#else
    /* MAKEWORD stores major in the low byte and minor in the high byte. */
    uint16_t major = (uint16_t)(requested_version & 0xffu);
    if (major < 1u) return AC1_WSAVERNOTSUPPORTED;
    static const uint16_t supported_versions[] = {
        0x0001u, /* 1.0 */
        0x0101u, /* 1.1 */
        0x0002u, /* 2.0 */
        0x0102u, /* 2.1 */
        0x0202u  /* 2.2 */
    };
    uint16_t highest = supported_versions[
        sizeof(supported_versions) / sizeof(supported_versions[0]) - 1u];
    uint16_t negotiated = 0u;
    for (size_t i = 0;
         i < sizeof(supported_versions) / sizeof(supported_versions[0]); ++i) {
        if (version_less_equal(supported_versions[i], requested_version))
            negotiated = supported_versions[i];
    }
    if (!negotiated) return AC1_WSAVERNOTSUPPORTED;
    memset(guest, 0, AC1_GUEST_WSADATA_SIZE);
    put_u16(guest, negotiated);
    put_u16(guest + 2u, highest);
    copy_terminated((char *)guest + AC1_GUEST_DESCRIPTION_OFFSET,
                    AC1_GUEST_DESCRIPTION_SIZE,
                    "POSIX sockets (Winsock-compatible API)", 40u);
    copy_terminated((char *)guest + AC1_GUEST_STATUS_OFFSET,
                    AC1_GUEST_STATUS_SIZE, "Initialized", 12u);
    /* Zero means no provider-specific maximum is reported. */
    put_u16(guest + AC1_GUEST_MAX_SOCKETS_OFFSET, 0u);
    put_u16(guest + AC1_GUEST_MAX_UDP_OFFSET, 0u);
    memset(guest + AC1_GUEST_VENDOR_POINTER_OFFSET, 0, 4u);
    if (g_startup_references != UINT32_MAX) ++g_startup_references;
    return 0u;
#endif
}

static uint32_t network_cleanup(void)
{
    if (!g_startup_references) {
        g_wsa_error = AC1_WSAENOTINITIALISED;
        return UINT32_MAX; /* SOCKET_ERROR */
    }

#ifdef _WIN32
    if (WSACleanup() == SOCKET_ERROR) {
        g_wsa_error = (uint32_t)WSAGetLastError();
        return UINT32_MAX;
    }
#endif
    --g_startup_references;
    return 0u;
}

int ac1_network_import(const char *dll_name, const char *name,
                       uint16_t ordinal, int by_ordinal,
                       const uint32_t *arguments, uint32_t *eax_out,
                       uint32_t *stack_bytes_out)
{
    if (!dll_name || !same_ascii_fold(dll_name, "WS2_32.dll") ||
        !eax_out || !stack_bytes_out)
        return 0;

    if ((by_ordinal && ordinal == 115u) ||
        (!by_ordinal && same_ascii_fold(name, "WSAStartup"))) {
        if (!arguments) return 0;
        *eax_out = network_startup((uint16_t)arguments[0], arguments[1]);
        *stack_bytes_out = 8u;
        return 1;
    }
    if ((by_ordinal && ordinal == 116u) ||
        (!by_ordinal && same_ascii_fold(name, "WSACleanup"))) {
        *eax_out = network_cleanup();
        *stack_bytes_out = 0u;
        return 1;
    }
    if ((by_ordinal && ordinal == 111u) ||
        (!by_ordinal && same_ascii_fold(name, "WSAGetLastError"))) {
        *eax_out = g_wsa_error;
        *stack_bytes_out = 0u;
        return 1;
    }
    if ((by_ordinal && ordinal == 112u) ||
        (!by_ordinal && same_ascii_fold(name, "WSASetLastError"))) {
        if (!arguments) return 0;
        g_wsa_error = arguments[0];
        *eax_out = 0u;
        *stack_bytes_out = 4u;
        return 1;
    }
    return 0;
}

uint32_t ac1_network_last_error(void)
{
    return g_wsa_error;
}

void ac1_network_set_last_error(uint32_t error)
{
    g_wsa_error = error;
}

void ac1_network_reset(void)
{
#ifdef _WIN32
    while (g_startup_references) {
        if (WSACleanup() == SOCKET_ERROR) break;
        --g_startup_references;
    }
#else
    g_startup_references = 0u;
#endif
    g_startup_references = 0u;
    g_wsa_error = 0u;
}
