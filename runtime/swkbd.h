// nn::swkbd::ShowKeyboard (swkbd.c) and its front end, the window that takes the text (swkbd_win.c with Win32,
// swkbd_sdl.c with SDL).
#pragma once
#include <stddef.h>
#include <stdint.h>

#define KB_LIMIT 500   // the applet's longest text

// The request, front-end independent.
typedef struct {
    uint16_t prompt[0x101];              // header, else guide, else sub text (UTF-16, NUL-terminated; may be empty)
    uint16_t initial[KB_LIMIT + 1];      // starting text (the game's, else the catalog's default name)
    int game_initial;                    // the starting text is the game's
    int max_len, min_len, password, numeric;
    uint32_t disabled;
} SwkbdRequest;

// Shows the text entry and waits for the answer, on the guest thread that called (its virtual core given up, the
// game's input blocked). Returns 1 with the text in out (n units), 0 when cancelled.
int text_entry_run(const SwkbdRequest *req, uint16_t *out, size_t cap, size_t *n);

// UTF-16 (n units) -> NUL-terminated UTF-8 in cap bytes, cut at a character boundary; the bytes written.
size_t swkbd_utf16_to_utf8(const uint16_t *s, size_t n, char *out, size_t cap);
// UTF-8 -> UTF-16, at most cap units (a character that does not fit whole is dropped); bad bytes become U+FFFD.
size_t swkbd_utf8_to_utf16(const char *s, uint16_t *out, size_t cap);
// Whether the applet would let this character through (single UTF-16 units). Disabled-key bits as the applet's: 1 space,
// 2 '@', 3 '%', 4 '/', 5 '\', 6 digits.
int swkbd_allowed(uint16_t c, uint32_t disabled, int digits_only);
