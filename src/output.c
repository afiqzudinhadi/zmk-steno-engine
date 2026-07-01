#include "output.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/hid.h>
#include <zmk/endpoints.h>
#include <zmk/events/keycode_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct hid_map {
    uint8_t keycode;
    bool shift;
};

static const struct hid_map ASCII_TO_HID[128] = {
    [' ']  = {0x2C, false},  /* space */
    ['!']  = {0x1E, true},   /* shift+1 */
    ['"']  = {0x34, true},   /* shift+' */
    ['#']  = {0x20, true},
    ['$']  = {0x21, true},
    ['%']  = {0x22, true},
    ['&']  = {0x24, true},
    ['\''] = {0x34, false},
    ['(']  = {0x26, true},
    [')']  = {0x27, true},
    ['*']  = {0x25, true},
    ['+']  = {0x2E, true},
    [',']  = {0x36, false},
    ['-']  = {0x2D, false},
    ['.']  = {0x37, false},
    ['/']  = {0x38, false},
    ['0']  = {0x27, false},
    ['1']  = {0x1E, false},
    ['2']  = {0x1F, false},
    ['3']  = {0x20, false},
    ['4']  = {0x21, false},
    ['5']  = {0x22, false},
    ['6']  = {0x23, false},
    ['7']  = {0x24, false},
    ['8']  = {0x25, false},
    ['9']  = {0x26, false},
    [':']  = {0x33, true},
    [';']  = {0x33, false},
    ['<']  = {0x36, true},
    ['=']  = {0x2E, false},
    ['>']  = {0x37, true},
    ['?']  = {0x38, true},
    ['@']  = {0x1F, true},
    ['A']  = {0x04, true},
    ['B']  = {0x05, true},
    ['C']  = {0x06, true},
    ['D']  = {0x07, true},
    ['E']  = {0x08, true},
    ['F']  = {0x09, true},
    ['G']  = {0x0A, true},
    ['H']  = {0x0B, true},
    ['I']  = {0x0C, true},
    ['J']  = {0x0D, true},
    ['K']  = {0x0E, true},
    ['L']  = {0x0F, true},
    ['M']  = {0x10, true},
    ['N']  = {0x11, true},
    ['O']  = {0x12, true},
    ['P']  = {0x13, true},
    ['Q']  = {0x14, true},
    ['R']  = {0x15, true},
    ['S']  = {0x16, true},
    ['T']  = {0x17, true},
    ['U']  = {0x18, true},
    ['V']  = {0x19, true},
    ['W']  = {0x1A, true},
    ['X']  = {0x1B, true},
    ['Y']  = {0x1C, true},
    ['Z']  = {0x1D, true},
    ['[']  = {0x2F, false},
    ['\\'] = {0x31, false},
    [']']  = {0x30, false},
    ['^']  = {0x23, true},
    ['_']  = {0x2D, true},
    ['`']  = {0x35, false},
    ['a']  = {0x04, false},
    ['b']  = {0x05, false},
    ['c']  = {0x06, false},
    ['d']  = {0x07, false},
    ['e']  = {0x08, false},
    ['f']  = {0x09, false},
    ['g']  = {0x0A, false},
    ['h']  = {0x0B, false},
    ['i']  = {0x0C, false},
    ['j']  = {0x0D, false},
    ['k']  = {0x0E, false},
    ['l']  = {0x0F, false},
    ['m']  = {0x10, false},
    ['n']  = {0x11, false},
    ['o']  = {0x12, false},
    ['p']  = {0x13, false},
    ['q']  = {0x14, false},
    ['r']  = {0x15, false},
    ['s']  = {0x16, false},
    ['t']  = {0x17, false},
    ['u']  = {0x18, false},
    ['v']  = {0x19, false},
    ['w']  = {0x1A, false},
    ['x']  = {0x1B, false},
    ['y']  = {0x1C, false},
    ['z']  = {0x1D, false},
    ['{']  = {0x2F, true},
    ['|']  = {0x31, true},
    ['}']  = {0x30, true},
    ['~']  = {0x35, true},
};

#define HID_BACKSPACE 0x2A
#define HID_RETURN    0x28
#define HID_LSHIFT    0xE1

static void tap_key(uint8_t keycode, bool shift)
{
    if (shift) {
        raise_zmk_keycode_state_changed((struct zmk_keycode_state_changed){
            .usage_page = 0x07, .keycode = HID_LSHIFT,
            .implicit_modifiers = 0, .explicit_modifiers = 0,
            .state = true, .timestamp = k_uptime_get()});
    }

    raise_zmk_keycode_state_changed((struct zmk_keycode_state_changed){
        .usage_page = 0x07, .keycode = keycode,
        .implicit_modifiers = 0, .explicit_modifiers = 0,
        .state = true, .timestamp = k_uptime_get()});

    raise_zmk_keycode_state_changed((struct zmk_keycode_state_changed){
        .usage_page = 0x07, .keycode = keycode,
        .implicit_modifiers = 0, .explicit_modifiers = 0,
        .state = false, .timestamp = k_uptime_get()});

    if (shift) {
        raise_zmk_keycode_state_changed((struct zmk_keycode_state_changed){
            .usage_page = 0x07, .keycode = HID_LSHIFT,
            .implicit_modifiers = 0, .explicit_modifiers = 0,
            .state = false, .timestamp = k_uptime_get()});
    }
}

void steno_output_send(const char *text, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') {
            tap_key(HID_RETURN, false);
            continue;
        }
        if (c >= 128 || ASCII_TO_HID[c].keycode == 0) {
            LOG_WRN("Skipping non-ASCII char 0x%02X", c);
            continue;
        }
        tap_key(ASCII_TO_HID[c].keycode, ASCII_TO_HID[c].shift);
    }
}

void steno_output_backspace(int count)
{
    for (int i = 0; i < count; i++) {
        tap_key(HID_BACKSPACE, false);
    }
}
