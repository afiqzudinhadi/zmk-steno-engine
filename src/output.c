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
#define HID_LCTRL     0xE0
#define HID_LALT      0xE2
#define HID_RALT      0xE6

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

static void press_key(uint8_t keycode)
{
    raise_zmk_keycode_state_changed((struct zmk_keycode_state_changed){
        .usage_page = 0x07, .keycode = keycode,
        .implicit_modifiers = 0, .explicit_modifiers = 0,
        .state = true, .timestamp = k_uptime_get()});
}

static void release_key(uint8_t keycode)
{
    raise_zmk_keycode_state_changed((struct zmk_keycode_state_changed){
        .usage_page = 0x07, .keycode = keycode,
        .implicit_modifiers = 0, .explicit_modifiers = 0,
        .state = false, .timestamp = k_uptime_get()});
}

/* Map hex digit (0-15) to HID keycode */
static uint8_t hex_to_hid(uint8_t nib)
{
    if (nib == 0) {
        return 0x27; /* '0' */
    }
    if (nib <= 9) {
        return 0x1E + (nib - 1); /* '1'-'9': 0x1E-0x26 */
    }
    return 0x04 + (nib - 10); /* 'a'-'f': 0x04-0x09 */
}

/* Tap hex digits of codepoint (variable width, skip leading zeros) */
static void tap_hex_digits(uint32_t codepoint)
{
    char buf[9];
    int n = 0;

    if (codepoint == 0) {
        tap_key(hex_to_hid(0), false);
        return;
    }

    /* Build hex digits in reverse */
    uint32_t cp = codepoint;
    while (cp > 0) {
        buf[n++] = cp & 0xF;
        cp >>= 4;
    }

    /* Tap in forward order */
    for (int i = n - 1; i >= 0; i--) {
        tap_key(hex_to_hid(buf[i]), false);
    }
}

void steno_output_unicode(uint32_t codepoint)
{
    if (codepoint > 0x10FFFF) {
        LOG_WRN("Invalid codepoint U+%06X", codepoint);
        return;
    }

#if IS_ENABLED(CONFIG_STENO_UNICODE_MODE_LINUX)
    /* IBus/GTK: Ctrl+Shift+U, hex digits, Return */
    press_key(HID_LCTRL);
    press_key(HID_LSHIFT);
    tap_key(0x18, false); /* 'u' */
    release_key(HID_LSHIFT);
    release_key(HID_LCTRL);
    tap_hex_digits(codepoint);
    tap_key(HID_RETURN, false);

#elif IS_ENABLED(CONFIG_STENO_UNICODE_MODE_MACOS)
    /* macOS Unicode Hex Input: hold Option, type 4+ hex digits */
    press_key(HID_LALT);

    /* Pad to at least 4 digits */
    char digits[8];
    int n = 0;
    uint32_t cp = codepoint;

    do {
        digits[n++] = cp & 0xF;
        cp >>= 4;
    } while (cp > 0);

    /* Pad to 4 */
    while (n < 4) {
        digits[n++] = 0;
    }

    for (int i = n - 1; i >= 0; i--) {
        tap_key(hex_to_hid(digits[i]), false);
    }

    release_key(HID_LALT);

#elif IS_ENABLED(CONFIG_STENO_UNICODE_MODE_WINC)
    /* WinCompose: tap RAlt, 'u', hex digits, Return */
    tap_key(HID_RALT, false);
    tap_key(0x18, false); /* 'u' */
    tap_hex_digits(codepoint);
    tap_key(HID_RETURN, false);

#else
    LOG_WRN("Unicode disabled, skipping U+%04X", codepoint);
#endif
}

/* Decode UTF-8 byte at text[i], write codepoint, return bytes consumed (0 on error) */
static int utf8_decode(const char *text, size_t len, size_t i, uint32_t *cp)
{
    unsigned char c = (unsigned char)text[i];

    if (c < 0x80) {
        *cp = c;
        return 1;
    }

    uint32_t codepoint;
    int expect; /* expected continuation bytes */

    if ((c & 0xE0) == 0xC0) {
        codepoint = c & 0x1F;
        expect = 1;
    } else if ((c & 0xF0) == 0xE0) {
        codepoint = c & 0x0F;
        expect = 2;
    } else if ((c & 0xF8) == 0xF0) {
        codepoint = c & 0x07;
        expect = 3;
    } else {
        return 0; /* invalid lead byte */
    }

    if (i + expect >= len) {
        return 0; /* truncated */
    }

    for (int j = 1; j <= expect; j++) {
        unsigned char cont = (unsigned char)text[i + j];
        if ((cont & 0xC0) != 0x80) {
            return 0;
        }
        codepoint = (codepoint << 6) | (cont & 0x3F);
    }

    /* Reject overlong encodings */
    if ((expect == 1 && codepoint < 0x80) ||
        (expect == 2 && codepoint < 0x800) ||
        (expect == 3 && codepoint < 0x10000)) {
        return 0;
    }

    /* Reject surrogates (U+D800..U+DFFF) and beyond Unicode max */
    if ((codepoint >= 0xD800 && codepoint <= 0xDFFF) || codepoint > 0x10FFFF) {
        return 0;
    }

    *cp = codepoint;
    return 1 + expect;
}

void steno_output_send(const char *text, size_t len)
{
    for (size_t i = 0; i < len; ) {
        unsigned char c = (unsigned char)text[i];

        if (c == '\n') {
            tap_key(HID_RETURN, false);
            i++;
            continue;
        }

        /* ASCII range */
        if (c < 128) {
            if (ASCII_TO_HID[c].keycode == 0) {
                LOG_WRN("Unmapped ASCII char 0x%02X", c);
                i++;
                continue;
            }
            tap_key(ASCII_TO_HID[c].keycode, ASCII_TO_HID[c].shift);
            i++;
            continue;
        }

        /* Multi-byte UTF-8 → Unicode codepoint */
        uint32_t codepoint;
        int consumed = utf8_decode(text, len, i, &codepoint);

        if (consumed == 0) {
            LOG_WRN("Invalid UTF-8 at offset %u (0x%02X)", (unsigned)i, c);
            i++;
            continue;
        }

        steno_output_unicode(codepoint);
        i += consumed;
    }
}

void steno_output_backspace(int count)
{
    for (int i = 0; i < count; i++) {
        tap_key(HID_BACKSPACE, false);
    }
}
