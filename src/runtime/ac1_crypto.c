#include "ac1_crypto.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
static SRWLOCK g_crypto_lock = SRWLOCK_INIT;
#define crypto_lock() AcquireSRWLockExclusive(&g_crypto_lock)
#define crypto_unlock() ReleaseSRWLockExclusive(&g_crypto_lock)
#else
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
static pthread_mutex_t g_crypto_lock = PTHREAD_MUTEX_INITIALIZER;
#define crypto_lock() ((void)pthread_mutex_lock(&g_crypto_lock))
#define crypto_unlock() ((void)pthread_mutex_unlock(&g_crypto_lock))
#endif

enum {
    AC1_CRYPTO_PROVIDER_CAPACITY = 16,
    AC1_CRYPTO_GUEST_HANDLE_BASE = 0xE5000000u,
    AC1_CRYPTO_GUEST_HANDLE_STRIDE = 4u,
    AC1_CRYPTO_PROVIDER_TYPE_RSA_FULL = 1u,
    AC1_CRYPTO_FLAGS_VERIFY_MACHINE_CONTEXT = 0xF0000020u,
    AC1_CRYPTO_FLAGS_VERIFY_MACHINE_NEWKEYSET = 0xF0000028u,
    AC1_ERROR_INVALID_HANDLE = 6u,
    AC1_ERROR_NOT_ENOUGH_MEMORY = 8u,
    AC1_ERROR_INVALID_PARAMETER = 87u,
    AC1_ERROR_ACCESS_DENIED = 5u,
    AC1_ERROR_FILE_NOT_FOUND = 2u,
    AC1_ERROR_RETRY = 1237u,
    AC1_ERROR_GEN_FAILURE = 31u,
    AC1_NTE_BAD_FLAGS = 0x80090009u,
    AC1_NTE_BAD_PROVIDER = 0x80090013u
};

typedef struct ac1_crypto_provider_slot {
    uint32_t guest_handle;
    uint8_t active;
#ifdef _WIN32
    HCRYPTPROV native_provider;
#endif
} ac1_crypto_provider_slot;

static ac1_crypto_provider_slot g_providers[AC1_CRYPTO_PROVIDER_CAPACITY];

static uint32_t slot_handle(size_t index)
{
    return AC1_CRYPTO_GUEST_HANDLE_BASE +
           (uint32_t)index * AC1_CRYPTO_GUEST_HANDLE_STRIDE;
}

static ac1_crypto_provider_slot *find_slot(uint32_t guest_handle)
{
    if (guest_handle < AC1_CRYPTO_GUEST_HANDLE_BASE) return NULL;
    uint32_t offset = guest_handle - AC1_CRYPTO_GUEST_HANDLE_BASE;
    if ((offset % AC1_CRYPTO_GUEST_HANDLE_STRIDE) != 0u) return NULL;
    size_t index = (size_t)(offset / AC1_CRYPTO_GUEST_HANDLE_STRIDE);
    if (index >= AC1_CRYPTO_PROVIDER_CAPACITY ||
        !g_providers[index].active)
        return NULL;
    return &g_providers[index];
}

#ifndef _WIN32
static uint32_t portable_entropy_error(int error)
{
    switch (error) {
    case EACCES:
#ifdef EPERM
    case EPERM:
#endif
        return AC1_ERROR_ACCESS_DENIED;
    case ENOENT:
        return AC1_ERROR_FILE_NOT_FOUND;
    case ENOMEM:
        return AC1_ERROR_NOT_ENOUGH_MEMORY;
    case EAGAIN:
        return AC1_ERROR_RETRY;
    default:
        return AC1_ERROR_GEN_FAILURE;
    }
}

static int open_entropy_device(uint32_t *error_out)
{
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int descriptor = open("/dev/urandom", flags);
    if (descriptor < 0 && error_out)
        *error_out = portable_entropy_error(errno);
    return descriptor;
}

static int fill_from_entropy(void *buffer, uint32_t length,
                             uint32_t *error_out)
{
    if (!length) return 1;
    if (!buffer) {
        if (error_out) *error_out = AC1_ERROR_INVALID_PARAMETER;
        return 0;
    }

    int descriptor = open_entropy_device(error_out);
    if (descriptor < 0) return 0;
    uint8_t *destination = (uint8_t *)buffer;
    uint32_t remaining = length;
    while (remaining) {
        ssize_t received = read(descriptor, destination, remaining);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) {
            int saved_error = received < 0 ? errno : EIO;
            (void)close(descriptor);
            if (error_out) *error_out = portable_entropy_error(saved_error);
            return 0;
        }
        destination += (size_t)received;
        remaining -= (uint32_t)received;
    }
    if (close(descriptor) != 0) {
        if (error_out) *error_out = portable_entropy_error(errno);
        return 0;
    }
    return 1;
}

static int portable_provider_supported(const char *container,
                                       const char *provider,
                                       uint32_t provider_type,
                                       uint32_t flags,
                                       uint32_t *error_out)
{
    if (container || !provider ||
        strcmp(provider, "Microsoft Base Cryptographic Provider v1.0") != 0 ||
        provider_type != AC1_CRYPTO_PROVIDER_TYPE_RSA_FULL) {
        if (error_out) *error_out = AC1_NTE_BAD_PROVIDER;
        return 0;
    }
    if (flags != AC1_CRYPTO_FLAGS_VERIFY_MACHINE_CONTEXT &&
        flags != AC1_CRYPTO_FLAGS_VERIFY_MACHINE_NEWKEYSET) {
        if (error_out) *error_out = AC1_NTE_BAD_FLAGS;
        return 0;
    }
    return 1;
}
#endif

int ac1_crypto_acquire(const char *container, const char *provider,
                       uint32_t provider_type, uint32_t flags,
                       uint32_t *guest_provider_out, uint32_t *error_out)
{
    if (guest_provider_out) *guest_provider_out = 0u;
    if (error_out) *error_out = 0u;
    if (!guest_provider_out || !error_out) return 0;

#ifndef _WIN32
    if (!portable_provider_supported(container, provider, provider_type,
                                     flags, error_out))
        return 0;
    /* VERIFYCONTEXT has no persistent container to open. Check that the real
     * OS entropy source is available before exposing an opaque provider token. */
    int entropy = open_entropy_device(error_out);
    if (entropy < 0) return 0;
    if (close(entropy) != 0) {
        *error_out = portable_entropy_error(errno);
        return 0;
    }
#endif

    crypto_lock();
    size_t slot_index = AC1_CRYPTO_PROVIDER_CAPACITY;
    for (size_t i = 0; i < AC1_CRYPTO_PROVIDER_CAPACITY; ++i) {
        if (!g_providers[i].active) {
            slot_index = i;
            break;
        }
    }
    if (slot_index == AC1_CRYPTO_PROVIDER_CAPACITY) {
        crypto_unlock();
        *error_out = AC1_ERROR_NOT_ENOUGH_MEMORY;
        return 0;
    }

#ifdef _WIN32
    HCRYPTPROV native_provider = 0;
    if (!CryptAcquireContextA(&native_provider, container, provider,
                              provider_type, flags)) {
        *error_out = (uint32_t)GetLastError();
        crypto_unlock();
        return 0;
    }
    if (!native_provider) {
        crypto_unlock();
        *error_out = AC1_ERROR_INVALID_HANDLE;
        return 0;
    }
    g_providers[slot_index].native_provider = native_provider;
#else
    (void)container;
    (void)provider;
    (void)provider_type;
    (void)flags;
#endif
    g_providers[slot_index].guest_handle = slot_handle(slot_index);
    g_providers[slot_index].active = 1u;
    *guest_provider_out = g_providers[slot_index].guest_handle;
    crypto_unlock();
    return 1;
}

int ac1_crypto_gen_random(uint32_t guest_provider, void *buffer,
                          uint32_t length, uint32_t *error_out)
{
    if (error_out) *error_out = 0u;
    if (!error_out) return 0;
    if (length && !buffer) {
        *error_out = AC1_ERROR_INVALID_PARAMETER;
        return 0;
    }

    crypto_lock();
    ac1_crypto_provider_slot *slot = find_slot(guest_provider);
    if (!slot) {
        crypto_unlock();
        *error_out = AC1_ERROR_INVALID_HANDLE;
        return 0;
    }
#ifdef _WIN32
    BOOL result = CryptGenRandom(slot->native_provider, length,
                                 (BYTE *)buffer);
    if (!result) *error_out = (uint32_t)GetLastError();
    crypto_unlock();
    return result != FALSE;
#else
    int result = fill_from_entropy(buffer, length, error_out);
    crypto_unlock();
    return result;
#endif
}

int ac1_crypto_release(uint32_t guest_provider, uint32_t flags,
                       uint32_t *error_out)
{
    if (error_out) *error_out = 0u;
    if (!error_out) return 0;

    crypto_lock();
    ac1_crypto_provider_slot *slot = find_slot(guest_provider);
    if (!slot) {
        crypto_unlock();
        *error_out = AC1_ERROR_INVALID_HANDLE;
        return 0;
    }
#ifdef _WIN32
    if (!CryptReleaseContext(slot->native_provider, flags)) {
        *error_out = (uint32_t)GetLastError();
        crypto_unlock();
        return 0;
    }
#else
    if (flags != 0u) {
        crypto_unlock();
        *error_out = AC1_ERROR_INVALID_PARAMETER;
        return 0;
    }
#endif
    memset(slot, 0, sizeof(*slot));
    crypto_unlock();
    return 1;
}

void ac1_crypto_reset(void)
{
    crypto_lock();
#ifdef _WIN32
    for (size_t i = 0; i < AC1_CRYPTO_PROVIDER_CAPACITY; ++i) {
        if (g_providers[i].active && g_providers[i].native_provider)
            (void)CryptReleaseContext(g_providers[i].native_provider, 0u);
    }
#endif
    memset(g_providers, 0, sizeof(g_providers));
    crypto_unlock();
}
