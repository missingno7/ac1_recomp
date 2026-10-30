#ifndef AC1_INPUT_SCRIPT_H
#define AC1_INPUT_SCRIPT_H
#include <stddef.h>
#include <stdint.h>
#include "ac1_gamepad.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ac1_input_script ac1_input_script;
typedef struct ac1_input_message {
    uint32_t message, wparam, lparam;
    uint8_t keyboard[256]; /* VK state at this event, including modifiers. */
    int32_t x, y;
    uint16_t text; /* ToUnicode result attached to this key event. */
    uint8_t text_valid;
} ac1_input_message;
/* Pure parser/state interface, also used by the contract tests. */
ac1_input_script *ac1_input_parse(const char *text, char *error, size_t capacity);
void ac1_input_free(ac1_input_script *script);
void ac1_input_advance(ac1_input_script *script, uint64_t present);
int ac1_input_pop(ac1_input_script *script, ac1_input_message *message);
void ac1_input_keyboard(ac1_input_script *script, uint8_t state[256]);
void ac1_input_pad(ac1_input_script *script, ac1_pad *out, int *connected, uint32_t *packet);
void ac1_input_mouse(ac1_input_script *script, void *state, size_t size);
int ac1_input_key_code(const char *name); /* DIK_NAME, NAME, or numeric DIK. */
uint32_t ac1_input_key_vk(unsigned dik);
uint32_t ac1_input_key_lparam(unsigned dik, int down, int previous);
/* Process session. Errors remain fatal to the bridge, never physical fallback. */
int ac1_input_session_init(char *error, size_t capacity);
void ac1_input_session_shutdown(void);
int ac1_input_replaying(void);
int ac1_input_replace_state(int keyboard, void *state, size_t size);
void ac1_input_record_state(int keyboard, const void *state, size_t size);
void ac1_input_record_message(uint32_t message, uint32_t wp, uint32_t lp);
void ac1_input_tick(void);
int ac1_input_next_message(ac1_input_message *message);
void ac1_input_cursor(int32_t *x, int32_t *y);
void ac1_input_vk_state(uint8_t state[256]);
/* UI-thread injection. DIK scan codes; ignored during scripted replay. */
void ac1_input_physical_key(unsigned dik, int down);
void ac1_input_physical_key_text(unsigned dik, int down, uint32_t codepoint);
/* AC1 accepts CP1252 Latin letters/digits, space, plus and minus. UTF-16
 * commits are atomic: 0 success, -1 unsupported, -2 queue full, -3 replay. */
int ac1_input_text_supported(uint32_t codepoint);
int ac1_input_commit_text(const uint16_t *text, size_t units);
int ac1_input_text_key(unsigned vk); /* Backspace=8, Enter=13, Escape=27. */
int ac1_input_text_translation(unsigned vk, uint16_t *text);
void ac1_input_physical_mouse(int32_t dx, int32_t dy, int32_t wheel,
                              unsigned buttons);
int ac1_input_physical_state(int keyboard, void *state, size_t size);
void ac1_input_physical_reset(void);
/* Apply the event snapshot only while executing its guest WndProc. */
const ac1_input_message *ac1_input_dispatch_state(const ac1_input_message *message);
#ifdef __cplusplus
}
#endif
#endif
