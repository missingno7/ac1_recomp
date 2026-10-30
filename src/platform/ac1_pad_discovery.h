#ifndef AC1_PAD_DISCOVERY_H
#define AC1_PAD_DISCOVERY_H
#include "ac1_gamepad.h"
#include <string.h>
/* AC1 constructs PadXenon only after DirectInput discovery reports VID/PID
 * 045e:028e (00916643..0091665a). This is a discovery node, not a DI gameplay
 * joystick: the original PadXenon polls XInput for all actual input. */
static const uint32_t ac1_pad_instance_guid[4] = {0xac100360u,0x4ca14673u,0x7ac1b883u,0x01000000u};
static inline int ac1_pad_is_discovery(const uint32_t guid[4]) {
    return !memcmp(guid,ac1_pad_instance_guid,16);
}
static inline int ac1_pad_discovery_connected(void) {
    ac1_pad_state state;
    return ac1_pad_get_state(0,&state)==0;
}
/* The selected input string is the product name passed by AC1 to 0090b2f0. */
#define AC1_PAD_PRODUCT_NAME "Controller (Xbox 360 For Windows)"
static inline void ac1_pad_discovery_record(void *buffer) {
    uint8_t *p=(uint8_t *)buffer;
    uint32_t size=0x44c, type=0x10515; /* DI8DEVTYPE_GAMEPAD, standard subtype, HID */
    uint32_t product[4]={0x028e045eu,0,0,0x44495650u};
    memset(p,0,size);memcpy(p,&size,4);memcpy(p+4,ac1_pad_instance_guid,16);
    memcpy(p+20,product,16);memcpy(p+36,&type,4);
    const char *label=AC1_PAD_PRODUCT_NAME;
    for(unsigned i=0;label[i];++i) { p[40+i*2]=(uint8_t)label[i];p[560+i*2]=(uint8_t)label[i]; }
}
#endif
