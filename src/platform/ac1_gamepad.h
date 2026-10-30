#ifndef AC1_GAMEPAD_H
#define AC1_GAMEPAD_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* XInput wire layout, little endian; no host pointers or imposed deadzones. */
typedef struct ac1_pad {
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
} ac1_pad;
typedef struct ac1_pad_state { uint32_t packet; ac1_pad pad; } ac1_pad_state;
typedef struct ac1_pad_caps {
    uint8_t type, subtype;
    uint16_t flags;
    ac1_pad pad;
    uint16_t low_motor, high_motor;
} ac1_pad_caps;
/* UI source supplies a complete snapshot. Disconnect clears all held controls. */
void ac1_pad_submit(unsigned user, int connected, const ac1_pad *pad);
void ac1_pad_reset(void);
uint32_t ac1_pad_get_state(unsigned user, ac1_pad_state *out);
uint32_t ac1_pad_set_state(unsigned user, const uint16_t motors[2]);
uint32_t ac1_pad_get_caps(unsigned user, unsigned flags, ac1_pad_caps *out);
/* Replay overrides physical input even before its first pad command. */
int ac1_input_session_pad(ac1_pad *out, int *connected, uint32_t *packet);
#ifdef __cplusplus
}
#endif
#endif
