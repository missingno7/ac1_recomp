#include "ac1_crypto.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const char provider[] = "Microsoft Base Cryptographic Provider v1.0";
    uint32_t guest_provider = 0u;
    uint32_t error = 0u;
    int acquired = ac1_crypto_acquire(NULL, provider, 1u, 0xF0000020u,
                                      &guest_provider, &error);
    if (!acquired) {
        acquired = ac1_crypto_acquire(NULL, provider, 1u, 0xF0000028u,
                                      &guest_provider, &error);
    }
    if (!acquired || !guest_provider) {
        fprintf(stderr, "CryptoAPI provider acquisition failed: 0x%08x\n",
                error);
        return 1;
    }

    struct { uint8_t before[16], output[32], after[16]; } guarded;
    memset(&guarded, 0xA5, sizeof(guarded));
    if (!ac1_crypto_gen_random(guest_provider, guarded.output,
                               sizeof(guarded.output), &error)) {
        fprintf(stderr, "CryptoAPI random generation failed: 0x%08x\n", error);
        ac1_crypto_reset();
        return 1;
    }
    unsigned changed = 0u;
    for (unsigned i = 0; i < sizeof(guarded.output); ++i)
        changed |= guarded.output[i] != 0xA5u;
    for (unsigned i = 0; i < sizeof(guarded.before); ++i) {
        if (guarded.before[i] != 0xA5u || guarded.after[i] != 0xA5u) {
            fprintf(stderr, "CryptoAPI wrote outside its output buffer\n");
            ac1_crypto_reset();
            return 1;
        }
    }
    /* Detect a no-op adapter; this is not a statistical randomness test. */
    if (!changed) {
        fprintf(stderr, "CryptoAPI left its output buffer unchanged\n");
        ac1_crypto_reset();
        return 1;
    }
    if (!ac1_crypto_release(guest_provider, 0u, &error)) {
        fprintf(stderr, "CryptoAPI provider release failed: 0x%08x\n", error);
        ac1_crypto_reset();
        return 1;
    }
    if (ac1_crypto_gen_random(guest_provider, guarded.output,
                              sizeof(guarded.output), &error) ||
        error != 6u) {
        fprintf(stderr, "released CryptoAPI guest handle was not rejected\n");
        ac1_crypto_reset();
        return 1;
    }

    ac1_crypto_reset();
    puts("CryptoAPI provider/random/release contract passed");
    return 0;
}
