// UTF-8 <-> UTF-16 conversion for the strings the guest keeps in UTF-16 (software keyboard, probes). Portable C, no
// OS calls; malformed UTF-8 bytes and unpaired surrogates become U+FFFD.
#pragma once
#include <stddef.h>
#include <stdint.h>

// utf8 into out as NUL-terminated UTF-16, at most cap units with the NUL; returns the units written before the NUL.
static inline size_t utf8_to_utf16(const char *utf8, uint16_t *out, size_t cap) {
    const unsigned char *s = (const unsigned char *)utf8;
    size_t n = 0;
    if (!cap) return 0;
    while (*s && n + 1 < cap) {
        uint32_t c = *s++;
        int more = c >= 0xf0 && c < 0xf8 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (c >= 0x80 && !more) c = 0xfffd;
        else if (more) c &= 0x3f >> more;
        for (; more; more--) {
            if ((*s & 0xc0) != 0x80) {
                c = 0xfffd;
                break;
            }
            c = c << 6 | (*s++ & 0x3f);
        }
        if (c > 0x10ffff || (c >= 0xd800 && c < 0xe000)) c = 0xfffd;
        if (c >= 0x10000) {
            if (n + 2 >= cap) break;
            c -= 0x10000;
            out[n++] = (uint16_t)(0xd800 + (c >> 10));
            out[n++] = (uint16_t)(0xdc00 + (c & 0x3ff));
        } else {
            out[n++] = (uint16_t)c;
        }
    }
    out[n] = 0;
    return n;
}

// NUL-terminated (or n units, whichever comes first) UTF-16 into out as NUL-terminated UTF-8, at most cap bytes with
// the NUL; returns the bytes written before the NUL.
static inline size_t utf16_to_utf8(const uint16_t *in, size_t n, char *out, size_t cap) {
    size_t k = 0;
    if (!cap) return 0;
    for (size_t i = 0; i < n && in[i]; i++) {
        uint32_t c = in[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && in[i + 1] >= 0xdc00 && in[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (in[++i] - 0xdc00);
        else if (c >= 0xd800 && c < 0xe000)
            c = 0xfffd;
        int len = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (k + (size_t)len >= cap) break;
        if (len == 1) out[k++] = (char)c;
        else {
            out[k++] = (char)((len == 2 ? 0xc0 : len == 3 ? 0xe0 : 0xf0) | (c >> (6 * (len - 1))));
            for (int j = len - 2; j >= 0; j--) out[k++] = (char)(0x80 | ((c >> (6 * j)) & 0x3f));
        }
    }
    out[k] = 0;
    return k;
}
