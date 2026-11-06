// Persian shaping and display order for the in-game menu (rtl_text.h). Our own code: a C port of the Persian text
// tools of the Poormaz Persian localization (persian_shaper.py), contributed to this project under its MIT licence.
//
// Shaping: a letter takes its initial, medial, final or isolated presentation form from whether it joins the letter
// before it and the one after it (Unicode chapter 9.2, Arabic cursive joining), lam + alef become one ligature, and
// the marks and invisible format characters ImGui would draw as boxes are dropped.
// Display order: a simplified form of the Unicode bidirectional algorithm (UAX #9) for one line of a right-to-left
// paragraph: weak types W1, W4-W7, paired brackets N0, neutrals N1-N2, then the line reversed with every run of
// Latin text or numbers kept in its own order and the brackets of right-to-left runs mirrored (L1-L4).
#include "rtl_text.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ---- shaping tables

// Presentation forms of the letters Persian text uses (Unicode's Arabic Presentation Forms-A and -B); 0: the letter
// has no such form. A letter with an initial or medial form joins on both sides, one with a final form only joins
// the letter before it (alef, dal, reh, waw, ...). Sorted by base letter.
typedef struct { uint16_t base, isolated, final, initial, medial; } Forms;
static const Forms kForms[] = {
    {0x0621, 0xFE80, 0, 0, 0},                    // ء
    {0x0622, 0xFE81, 0xFE82, 0, 0},               // آ
    {0x0623, 0xFE83, 0xFE84, 0, 0},               // أ
    {0x0624, 0xFE85, 0xFE86, 0, 0},               // ؤ
    {0x0625, 0xFE87, 0xFE88, 0, 0},               // إ
    {0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C},     // ئ
    {0x0627, 0xFE8D, 0xFE8E, 0, 0},               // ا
    {0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92},     // ب
    {0x0629, 0xFE93, 0xFE94, 0, 0},               // ة
    {0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98},     // ت
    {0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C},     // ث
    {0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0},     // ج
    {0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4},     // ح
    {0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8},     // خ
    {0x062F, 0xFEA9, 0xFEAA, 0, 0},               // د
    {0x0630, 0xFEAB, 0xFEAC, 0, 0},               // ذ
    {0x0631, 0xFEAD, 0xFEAE, 0, 0},               // ر
    {0x0632, 0xFEAF, 0xFEB0, 0, 0},               // ز
    {0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4},     // س
    {0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8},     // ش
    {0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC},     // ص
    {0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0},     // ض
    {0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4},     // ط
    {0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8},     // ظ
    {0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC},     // ع
    {0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0},     // غ
    {0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4},     // ف
    {0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8},     // ق
    {0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0},     // ل
    {0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4},     // م
    {0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8},     // ن
    {0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC},     // ه
    {0x0648, 0xFEED, 0xFEEE, 0, 0},               // و
    {0x067E, 0xFB56, 0xFB57, 0xFB58, 0xFB59},     // پ
    {0x0686, 0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D},     // چ
    {0x0698, 0xFB8A, 0xFB8B, 0, 0},               // ژ
    {0x06A9, 0xFB8E, 0xFB8F, 0xFB90, 0xFB91},     // ک
    {0x06AF, 0xFB92, 0xFB93, 0xFB94, 0xFB95},     // گ
    {0x06C0, 0xFBA4, 0xFBA5, 0, 0},               // ۀ
    {0x06CC, 0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF},     // ی
};

// lam + alef ligatures: the alef, then the isolated and the final form of the pair
static const uint16_t kLamAlef[][3] = {
    {0x0622, 0xFEF5, 0xFEF6}, {0x0623, 0xFEF7, 0xFEF8}, {0x0625, 0xFEF9, 0xFEFA}, {0x0627, 0xFEFB, 0xFEFC},
};

enum { LAM = 0x0644, ZWNJ = 0x200C, ZWJ = 0x200D, TATWEEL = 0x0640, HAMZA_ABOVE = 0x0654 };

static const Forms *forms_of(uint32_t c) {
    int lo = 0, hi = (int)(sizeof kForms / sizeof *kForms) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (kForms[mid].base == c) return &kForms[mid];
        if (kForms[mid].base < c) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

// ---- UTF-8

static uint32_t utf8_next(const char **ps, const char *end) {
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t c = s[0];
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!n || (const char *)s + n > end) {
        *ps += 1;
        return 0xFFFD;
    }
    if (n > 1) {
        c &= 0x7F >> n;
        for (int i = 1; i < n; i++) {
            if ((s[i] & 0xC0) != 0x80) {
                *ps += 1;
                return 0xFFFD;
            }
            c = c << 6 | (s[i] & 0x3F);
        }
    }
    *ps += n;
    return c;
}

static size_t utf8_len(uint32_t c) { return c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4; }

static size_t utf8_put(uint32_t c, char *o) {
    unsigned char *u = (unsigned char *)o;
    if (c < 0x80) {
        u[0] = (unsigned char)c;
        return 1;
    }
    if (c < 0x800) {
        u[0] = (unsigned char)(0xC0 | c >> 6), u[1] = (unsigned char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        u[0] = (unsigned char)(0xE0 | c >> 12), u[1] = (unsigned char)(0x80 | (c >> 6 & 0x3F));
        u[2] = (unsigned char)(0x80 | (c & 0x3F));
        return 3;
    }
    u[0] = (unsigned char)(0xF0 | c >> 18), u[1] = (unsigned char)(0x80 | (c >> 12 & 0x3F));
    u[2] = (unsigned char)(0x80 | (c >> 6 & 0x3F)), u[3] = (unsigned char)(0x80 | (c & 0x3F));
    return 4;
}

// Decodes [s, end) into a code point array: the caller's buffer when it is large enough, else a malloc'd one (freed
// with code_points_free). NULL when out of memory.
static uint32_t *code_points(const char *s, const char *end, uint32_t *buf, size_t buf_n, size_t *n) {
    size_t cap = (size_t)(end - s) + 1;
    uint32_t *cp = cap <= buf_n ? buf : (uint32_t *)malloc(cap * sizeof *cp);
    if (!cp) return NULL;
    size_t k = 0;
    while (s < end) cp[k++] = utf8_next(&s, end);
    *n = k;
    return cp;
}

static void code_points_free(uint32_t *cp, const uint32_t *buf) {
    if (cp != buf) free(cp);
}

// ---- character classes

static int in_arabic_block(uint32_t c) {
    return (c >= 0x0600 && c <= 0x06FF) || (c >= 0xFB50 && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}

static int is_digit_ar(uint32_t c) { return (c >= 0x0660 && c <= 0x0669) || (c >= 0x06F0 && c <= 0x06F9); }

// Vowel marks and other combining marks of the Arabic block (dropped: ImGui does not position marks).
static int is_arabic_mark(uint32_t c) {
    return (c >= 0x0610 && c <= 0x061A) || (c >= 0x064B && c <= 0x065F) || c == 0x0670 ||
           (c >= 0x06D6 && c <= 0x06DC) || (c >= 0x06DF && c <= 0x06E4) || c == 0x06E7 || c == 0x06E8 ||
           (c >= 0x06EA && c <= 0x06ED);
}

// Invisible format characters (zero-width space and joiners, direction marks and embeddings, soft hyphen, BOM):
// they only steer joining and direction, and the menu font has no glyph for them.
static int is_format(uint32_t c) {
    return (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x2064) ||
           (c >= 0x2066 && c <= 0x2069) || c == 0xFEFF || c == 0x00AD;
}

// Cursive joining type: 'D' joins both sides, 'R' only the letter before it, 'C' causes joining (ZWJ, tatweel),
// 'T' is transparent (marks, format characters), 'U' does not join (ZWNJ, everything else).
static char joining_type(uint32_t c) {
    if (c == ZWJ || c == TATWEEL) return 'C';
    if (c == ZWNJ) return 'U';
    const Forms *f = forms_of(c);
    if (f) return f->initial || f->medial ? 'D' : f->final ? 'R' : 'U';
    if (is_format(c) || (c >= 0x0300 && c <= 0x036F) || is_arabic_mark(c)) return 'T';
    return 'U';
}

int rtl_has(const char *s, const char *end) {
    if (!end) end = s + strlen(s);
    while (s < end) {
        if ((unsigned char)*s < 0x80) {
            s++;
            continue;
        }
        uint32_t c = utf8_next(&s, end);
        if (in_arabic_block(c) && !is_digit_ar(c)) return 1;
    }
    return 0;
}

// ---- shaping

// Arabic letters written as their Persian twins, heh / yeh / waw / alef + hamza above as one letter, Arabic-Indic
// digits as Persian digits, vowel marks dropped. In place; returns the new count.
static size_t normalise(uint32_t *cp, size_t n) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = cp[i];
        if (c == HAMZA_ABOVE) {
            if (k) {
                uint32_t p = cp[k - 1];
                cp[k - 1] = p == 0x0647 || p == 0x06D5 ? 0x06C0 : p == 0x06CC ? 0x0626 : p == 0x0648 ? 0x0624
                          : p == 0x0627 ? 0x0623 : p;
            }
            continue;
        }
        if (is_arabic_mark(c)) continue;
        if (c == 0x064A || c == 0x0649) c = 0x06CC;          // yeh, alef maksura -> Farsi yeh
        else if (c == 0x0643) c = 0x06A9;                     // kaf -> keheh
        else if (c == 0x06D5) c = 0x0647;                     // ae -> heh
        else if (c >= 0x0660 && c <= 0x0669) c += 0x06F0 - 0x0660;
        cp[k++] = c;
    }
    return k;
}

size_t rtl_shape(const char *s, char *out, size_t cap) {
    if (!cap) return 0;
    const char *end = s + strlen(s);
    if (!rtl_has(s, end)) {
        size_t len = (size_t)(end - s) < cap - 1 ? (size_t)(end - s) : cap - 1;
        memcpy(out, s, len);
        out[len] = 0;
        return len;
    }
    uint32_t buf[512];
    size_t n = 0;
    uint32_t *cp = code_points(s, end, buf, sizeof buf / sizeof *buf, &n);
    if (!cp) {
        out[0] = 0;
        return 0;
    }
    n = normalise(cp, n);
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = cp[i], v = c;
        char t = joining_type(c);
        if (c == ZWNJ || c == ZWJ || is_format(c)) continue;
        if (t == 'D' || t == 'R') {
            size_t p = i;   // the nearest letter before, past transparent ones, that joins towards this one
            while (p > 0 && joining_type(cp[p - 1]) == 'T') p--;
            int joins_prev = p > 0 && (joining_type(cp[p - 1]) == 'D' || joining_type(cp[p - 1]) == 'C');
            size_t q = i + 1;
            while (q < n && joining_type(cp[q]) == 'T') q++;
            int lam_alef = -1;
            if (c == LAM && q < n)
                for (int a = 0; a < 4; a++)
                    if (cp[q] == kLamAlef[a][0]) lam_alef = a;
            if (lam_alef >= 0) {
                v = kLamAlef[lam_alef][joins_prev ? 2 : 1];
                i = q;   // the alef is part of the ligature
            } else {
                char tn = q < n ? joining_type(cp[q]) : 'U';
                int joins_next = t == 'D' && (tn == 'D' || tn == 'R' || tn == 'C');
                const Forms *f = forms_of(c);
                uint16_t form = joins_prev && joins_next ? f->medial : joins_prev ? f->final
                              : joins_next ? f->initial : f->isolated;
                v = form ? form : f->isolated ? f->isolated : c;
            }
        }
        if (o + utf8_len(v) + 1 > cap) break;
        o += utf8_put(v, out + o);
    }
    out[o] = 0;
    code_points_free(cp, buf);
    return o;
}

// ---- display order

enum { B_L, B_R, B_EN, B_ES, B_ET, B_CS, B_WS, B_ON, B_NSM };

static int bidi_class(uint32_t c) {
    if (in_arabic_block(c)) {
        if (is_digit_ar(c)) return B_EN;
        if (c == 0x066A) return B_ET;                                      // ٪
        if (c == 0x060C || c == 0x061B || c == 0x061F || c == 0x066B || c == 0x066C) return B_ON;   // ، ؛ ؟ ٫ ٬
        return B_R;
    }
    if (c < 0x80) {
        if (c >= '0' && c <= '9') return B_EN;
        if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') return B_L;
        if (c == '+' || c == '-') return B_ES;
        if (c == '#' || c == '$' || c == '%') return B_ET;
        if (c == ',' || c == '.' || c == '/' || c == ':') return B_CS;
        if (c == ' ' || c == '\t') return B_WS;
        return B_ON;
    }
    if (c == 0x00A0) return B_CS;
    if ((c >= 0x00A2 && c <= 0x00A5) || c == 0x00B0 || c == 0x00B1 || c == 0x2030 || (c >= 0x20A0 && c <= 0x20CF))
        return B_ET;
    if (c >= 0x00C0 && c <= 0x024F && c != 0x00D7 && c != 0x00F7) return B_L;
    if (c >= 0x0300 && c <= 0x036F) return B_NSM;
    if (c >= 0x0370 && c <= 0x058F) return B_L;                            // Greek, Cyrillic, Armenian
    if ((c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFB4F)) return B_R;   // Hebrew, Syriac, Thaana, ...
    if ((c >= 0x2000 && c <= 0x200A) || c == 0x3000) return B_WS;
    if (c < 0x0300 || (c >= 0x2000 && c <= 0x2BFF) || (c >= 0x3001 && c <= 0x303F) || (c >= 0xFE10 && c <= 0xFE6F) ||
        (c >= 0xFF00 && c <= 0xFF20))
        return B_ON;
    return B_L;
}

static uint32_t mirror(uint32_t c) {
    switch (c) {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '{': return '}';
    case '}': return '{';
    case '<': return '>';
    case '>': return '<';
    case 0x00AB: return 0x00BB;
    case 0x00BB: return 0x00AB;
    case 0x2039: return 0x203A;
    case 0x203A: return 0x2039;
    default: return c;
    }
}

static uint32_t closing_of(uint32_t c) {
    return c == '(' ? ')' : c == '[' ? ']' : c == '{' ? '}' : c == 0x00AB ? 0x00BB : 0;
}

// Level of every code point of an RTL line: 1 (right to left) or 2 (a run of Latin text or numbers). cls is
// rewritten. Strong 'R' and numbers both count as right-to-left context for the neutrals between them.
static void resolve_levels(const uint32_t *cp, unsigned char *cls, unsigned char *level, size_t n) {
    for (size_t i = 0; i < n; i++)   // W1: a mark takes the class before it
        if (cls[i] == B_NSM) cls[i] = i ? cls[i - 1] : B_R;
    for (size_t i = 1; i + 1 < n; i++)   // W4: one separator between digits
        if ((cls[i] == B_ES || cls[i] == B_CS) && cls[i - 1] == B_EN && cls[i + 1] == B_EN) cls[i] = B_EN;
    for (size_t i = 0; i < n;) {   // W5: terminators next to digits
        if (cls[i] != B_ET) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < n && cls[j] == B_ET) j++;
        if ((i > 0 && cls[i - 1] == B_EN) || (j < n && cls[j] == B_EN))
            for (size_t k = i; k < j; k++) cls[k] = B_EN;
        i = j;
    }
    for (size_t i = 0; i < n; i++)   // W6
        if (cls[i] == B_ES || cls[i] == B_ET || cls[i] == B_CS) cls[i] = B_ON;
    int last = B_R;   // W7: digits after Latin text belong to it
    for (size_t i = 0; i < n; i++) {
        if (cls[i] == B_L || cls[i] == B_R) last = cls[i];
        else if (cls[i] == B_EN && last == B_L) cls[i] = B_L;
    }
    // N0: a bracket pair takes the direction of the text inside it (R when any, else L when the text before the
    // pair is L, else R). Pairs are matched with a stack (an unmatched closing bracket is left alone), then resolved
    // in the order of their opening brackets, outer pairs before the pairs inside them.
    enum { DEPTH = 63, PAIRS = 64 };
    size_t open_at[DEPTH], pair_a[PAIRS], pair_b[PAIRS];
    uint32_t open_close[DEPTH];
    int depth = 0, pairs = 0;
    for (size_t i = 0; i < n; i++) {
        if (cls[i] != B_ON) continue;
        uint32_t close = closing_of(cp[i]);
        if (close) {
            if (depth < DEPTH) open_at[depth] = i, open_close[depth] = close, depth++;
            continue;
        }
        for (int k = depth - 1; k >= 0; k--) {
            if (open_close[k] != cp[i]) continue;
            if (pairs < PAIRS) pair_a[pairs] = open_at[k], pair_b[pairs] = i, pairs++;
            depth = k;
            break;
        }
    }
    for (int p = 1; p < pairs; p++)   // insertion sort by opening bracket (a handful of pairs per line)
        for (int q = p; q > 0 && pair_a[q - 1] > pair_a[q]; q--) {
            size_t ta = pair_a[q], tb = pair_b[q];
            pair_a[q] = pair_a[q - 1], pair_b[q] = pair_b[q - 1];
            pair_a[q - 1] = ta, pair_b[q - 1] = tb;
        }
    for (int p = 0; p < pairs; p++) {
        size_t a = pair_a[p], b = pair_b[p];
        int has_r = 0, has_l = 0;
        for (size_t m = a + 1; m < b; m++) {
            has_r |= cls[m] == B_R || cls[m] == B_EN;
            has_l |= cls[m] == B_L;
        }
        if (!has_r && !has_l) continue;
        int d = B_R;
        if (!has_r) {
            int prev = B_R;
            for (size_t m = a; m-- > 0;)
                if (cls[m] == B_L || cls[m] == B_R || cls[m] == B_EN) {
                    prev = cls[m] == B_L ? B_L : B_R;
                    break;
                }
            d = prev;
        }
        cls[a] = cls[b] = (unsigned char)d;
    }
    // N1 / N2: neutrals between two Latin runs are Latin, all others follow the paragraph (right to left)
    for (size_t i = 0; i < n;) {
        if (cls[i] == B_L || cls[i] == B_R || cls[i] == B_EN) {
            level[i] = cls[i] == B_R ? 1 : 2;
            i++;
            continue;
        }
        size_t j = i;
        while (j < n && !(cls[j] == B_L || cls[j] == B_R || cls[j] == B_EN)) j++;
        int left = i > 0 && cls[i - 1] == B_L, right = j < n && cls[j] == B_L;
        for (size_t k = i; k < j; k++) level[k] = left && right ? 2 : 1;
        i = j;
    }
}

size_t rtl_visual(const char *s, const char *end, char *out, size_t cap) {
    if (!cap) return 0;
    size_t len = (size_t)(end - s);
    if (!rtl_has(s, end)) {
        if (len > cap - 1) len = cap - 1;
        memcpy(out, s, len);
        out[len] = 0;
        return len;
    }
    uint32_t buf[512];
    unsigned char cls_buf[512], lvl_buf[512];
    size_t n = 0;
    uint32_t *cp = code_points(s, end, buf, sizeof buf / sizeof *buf, &n);
    unsigned char *cls = n <= sizeof cls_buf ? cls_buf : (unsigned char *)malloc(n);
    unsigned char *level = n <= sizeof lvl_buf ? lvl_buf : (unsigned char *)malloc(n);
    size_t o = 0;
    if (cp && cls && level) {
        for (size_t i = 0; i < n; i++) cls[i] = (unsigned char)bidi_class(cp[i]);
        resolve_levels(cp, cls, level, n);
        // the line reversed; each run of level 2 then turned back into its own order
        for (size_t k = n; k > 0;) {
            size_t i = k - 1;
            if (level[i] == 2) {
                size_t a = i;
                while (a > 0 && level[a - 1] == 2) a--;
                for (size_t m = a; m <= i; m++) {
                    if (o + utf8_len(cp[m]) + 1 > cap) goto done;
                    o += utf8_put(cp[m], out + o);
                }
                k = a;
            } else {
                uint32_t c = mirror(cp[i]);
                if (o + utf8_len(c) + 1 > cap) goto done;
                o += utf8_put(c, out + o);
                k = i;
            }
        }
    }
done:
    out[o] = 0;
    if (cls != cls_buf) free(cls);
    if (level != lvl_buf) free(level);
    if (cp) code_points_free(cp, buf);
    return o;
}
