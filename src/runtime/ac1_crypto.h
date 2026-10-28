#ifndef AC1_CRYPTO_H
#define AC1_CRYPTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Narrow CryptoAPI surface used by the pinned AC1 startup path. Input strings
 * and output buffers are host views of checked guest memory, not guest VAs.
 * Provider handles returned here are opaque 32-bit guest tokens and are never
 * host HCRYPTPROV values.
 *
 * Functions return nonzero on success. `error_out` receives a Win32/NTE error
 * on failure and zero on success. The provider table is bounded; handles stay
 * live until release or reset.
 */
int ac1_crypto_acquire(const char *container, const char *provider,
                       uint32_t provider_type, uint32_t flags,
                       uint32_t *guest_provider_out, uint32_t *error_out);
int ac1_crypto_gen_random(uint32_t guest_provider, void *buffer,
                          uint32_t length, uint32_t *error_out);
int ac1_crypto_release(uint32_t guest_provider, uint32_t flags,
                       uint32_t *error_out);

/* Release any native contexts owned by this module and invalidate all tokens. */
void ac1_crypto_reset(void);

#ifdef __cplusplus
}
#endif

#endif
