// The port's settings as rows of TotK 1.4.3's own options screen (SystemOption_00, the title menu's and the pause
// menu's "Options"): the game builds, draws, moves the cursor over and plays the sounds of these rows like its own;
// the port only gives it the rows, their texts and their values.
//
// How the game's screen works (1.4.3 main, read from the code):
//  - The rows come from UI/OptionSetTable/OptionSetTable.game__ui__OptionSetTable.bgyml (in the bootup pack), held
//    as a list at +0x5e8 of the object at main+0x3a82e70: { u32 count; Item *items } with 0x28-byte items:
//      +0x00 u32 OptionType (IsUseAmiibo 0 .. IsUseApp; None, a separator's, is -1; it numbers the texts below)
//      +0x04 u32 ButtonType (0 Toggle, 1 Selection, 2 Button, 3 Separator), +0x08 u32 the item's index in the list
//      +0x10 u32 choice count, +0x18 u32 *choices (ChoiceType values; their names at main+0x3718f18)
//      +0x20 u32 the GameData key of the value (a hash), +0x24 u32 CheckGameData (0: always shown)
//    main+0x2089c14 counts the shown items and main+0x2089cb4 returns the i-th; the screen's builder (main+0x218bb10)
//    clones the template rows (Pa_ToggleBtn_00, Pa_SelectBtn_00, Pa_Separator_00) once per item.
//  - Texts: LayoutMsg/SystemOption_00 through main+0xba42c0 (handle, &archive, &label, 1): a row's label "10%02d"
//    (OptionType), a choice "10%02d_%02d" (OptionType, choice index), the description "%04d" (OptionType).
//  - Values: GameData by the item's key. The toggle row reads it with main+0x9eee70 (bool) and writes it with
//    main+0x923d20; the selection row reads an enum (main+0x980288, the hash of the choice's name, main+0x7d4310) and
//    writes it with main+0x170210.
// The patch set "gamemenu" (recomp/patches-v143.txt) sends those calls of the screen to the host functions below: the
// list count adds the port's items to the list (once), and the texts and values of the port's items come from the
// port's settings (cfg.h) and its text catalog (ui_text.h). Everything else goes on to the game's functions.
//
// TOTK_GAME_MENU (the patch set's switch, default 1): 0 leaves the game's options screen as it is.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "cpu.h"
#include "dispatch.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"
#include "ui_text.h"
#if __has_include("patches.h")
#include "patches.h"
#endif

#ifdef RT_PATCH_PAGE
#define G(off) (MOD_MAIN_BASE + (off))
#define G_LIST_COUNT G(0x2089c14ull)
#define G_MSG_GET G(0xba42c0ull)
#define G_GET_BOOL G(0x9eee70ull)
#define G_SET_BOOL G(0x923d20ull)
#define G_GET_ENUM G(0x980288ull)
#define G_SET_ENUM G(0x170210ull)
#define G_NAME_HASH G(0x7d4310ull)
#define G_CHOICE_NAMES G(0x3718f18ull)   // const char *[20]: UseAmiibo .. UnuseApp
#define N_CHOICE_NAMES 20

#define HOST_LIST (RT_PATCH_PAGE + 0xf00)
#define HOST_MSG (RT_PATCH_PAGE + 0xf10)
#define HOST_GET_BOOL (RT_PATCH_PAGE + 0xf20)
#define HOST_SET_BOOL (RT_PATCH_PAGE + 0xf30)
#define HOST_GET_ENUM (RT_PATCH_PAGE + 0xf40)
#define HOST_SET_ENUM (RT_PATCH_PAGE + 0xf50)

#define ITEM_SIZE 0x28
#define TYPE_BASE 20               // the port's OptionType values: past the game's 17, below 100 (the texts are "10%02d")
#define KEY_BASE 0x50430000u       // the port's GameData keys ("PC"): no hash the game's data uses
#define MAX_CHOICES N_CHOICE_NAMES // a selection's choice is a ChoiceType value: distinct names up to 20

static uint64_t rd64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
static uint32_t rd32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static void wr32(uint64_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, 4); }
static void wr64(uint64_t a, uint64_t v) { memcpy((void *)(uintptr_t)a, &v, 8); }

#include "game_menu_rows.h"

// ---- the list: the game's items, then the port's
static uint64_t g_items;   // the merged array once made
static PlatMutex g_lock = PLAT_MUTEX_INIT;

static void extend(uint64_t list) {
    if (!list || (g_items && rd64(list + 8) == g_items)) return;
    plat_mutex_lock(&g_lock);
    if (!g_items || rd64(list + 8) != g_items) {
        if (!n_rows) rows_init();
        uint32_t n = rd32(list);
        uint64_t old = rd64(list + 8);
        static int logged;
        if (!logged++) {
            rt_log("game menu: the options list has %u items at %#llx", n, (unsigned long long)old);
            for (uint32_t i = 0; i < n && i < 32; i++) {
                const uint32_t *w = (const uint32_t *)(uintptr_t)(old + (uint64_t)i * ITEM_SIZE);
                rt_log("game menu:  item %2u: type %u button %u +8 %#x +c %#x choices %u +20 %#x +24 %#x", i, w[0], w[1],
                       w[2], w[3], w[4], w[8], w[9]);
            }
        }
        uint8_t *items = mem_kernel_alloc((uint64_t)(n + n_rows) * ITEM_SIZE, 16);
        uint32_t *choices = mem_kernel_alloc(MAX_CHOICES * 4, 16);
        if (!items || !choices) {
            plat_mutex_unlock(&g_lock);
            rt_log("game menu: no memory for the options list");
            return;
        }
        for (int i = 0; i < MAX_CHOICES; i++) choices[i] = (uint32_t)i;
        memcpy(items, (const void *)(uintptr_t)old, (size_t)n * ITEM_SIZE);
        for (int i = 0; i < n_rows; i++) {
            uint8_t *it = items + (size_t)(n + i) * ITEM_SIZE;
            // a separator's OptionType is None (-1), as the game's are; +8 numbers the items in list order
            wr32((uint64_t)(uintptr_t)it + 0x00, rows[i].kind == ROW_SEPARATOR ? 0xffffffffu : (uint32_t)(TYPE_BASE + i));
            wr32((uint64_t)(uintptr_t)it + 0x04, (uint32_t)rows[i].kind);
            wr32((uint64_t)(uintptr_t)it + 0x08, n + (uint32_t)i);
            wr32((uint64_t)(uintptr_t)it + 0x10, rows[i].kind == ROW_CHOICE ? (uint32_t)rows[i].n : 0);
            wr64((uint64_t)(uintptr_t)it + 0x18, rows[i].kind == ROW_CHOICE ? (uint64_t)(uintptr_t)choices : 0);
            wr32((uint64_t)(uintptr_t)it + 0x20, rows[i].kind == ROW_SEPARATOR ? 0 : KEY_BASE + (uint32_t)i);
        }
        g_items = (uint64_t)(uintptr_t)items;
        wr64(list + 8, g_items);
        wr32(list, n + (uint32_t)n_rows);
        rt_log("game menu: %d rows of the port's settings after the game's %u", n_rows, n);
    }
    plat_mutex_unlock(&g_lock);
}

static void host_list(Cpu *c) {
    extend(c->x[0]);
    c->x[0] = rt_call(c, G_LIST_COUNT, 1, c->x[0]);
}

// ---- texts: UTF-16 copies in guest memory, one per label, renewed when the text changes (another language)
typedef struct {
    char label[16];
    uint32_t hash;
    uint64_t text;
    uint32_t len;
} Text;
static Text texts[512];
static int n_texts;

static uint32_t fnv(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static uint64_t utf16(const char *s, uint32_t *len) {
    size_t n = strlen(s);
    uint16_t *out = mem_kernel_alloc((n + 1) * 2, 16);
    if (!out) return 0;
    size_t k = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p;) {
        uint32_t cp = *p++;
        if (cp >= 0xc0) {
            int more = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : 1;
            cp &= 0x3f >> more;
            for (; more && (*p & 0xc0) == 0x80; more--) cp = cp << 6 | (*p++ & 0x3f);
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[k++] = (uint16_t)(0xd800 + (cp >> 10));
            out[k++] = (uint16_t)(0xdc00 + (cp & 0x3ff));
        } else {
            out[k++] = (uint16_t)cp;
        }
    }
    out[k] = 0;
    *len = (uint32_t)k;
    return (uint64_t)(uintptr_t)out;
}

static const Text *text_for(const char *label, const char *s) {
    uint32_t h = fnv(s);
    plat_mutex_lock(&g_lock);
    Text *t = NULL;
    for (int i = 0; i < n_texts && !t; i++)
        if (!strcmp(texts[i].label, label)) t = &texts[i];
    if (!t && n_texts < (int)(sizeof texts / sizeof *texts)) {
        t = &texts[n_texts++];
        snprintf(t->label, sizeof t->label, "%s", label);
        t->text = 0;
    }
    if (t && (!t->text || t->hash != h)) {
        t->text = utf16(s, &t->len);
        t->hash = h;
    }
    plat_mutex_unlock(&g_lock);
    return t && t->text ? t : NULL;
}

// The text of one of the port's labels, NULL for the game's own
static const char *port_text(const char *label, char *buf, size_t n) {
    int a = -1, b = -1;
    char tail;
    if (strlen(label) == 4 && sscanf(label, "10%2d%c", &a, &tail) == 1) {        // "10NN": a row's label
        const Row *r = row_of_type(a);
        return r ? menu_text(r->label) : NULL;
    }
    if (sscanf(label, "10%2d_%2d%c", &a, &b, &tail) == 2) {                       // "10NN_MM": a choice
        const Row *r = row_of_type(a);
        if (!r || b < 0 || b >= r->n) return NULL;
        return r->names[b] ? menu_text(r->names[b]) : r->lit[b];
    }
    if (strlen(label) == 4 && sscanf(label, "%4d%c", &a, &tail) == 1) {          // "NNNN": the description
        const Row *r = row_of_type(a);
        return r ? desc_of(r, buf, n) : NULL;
    }
    return NULL;
}

static void host_msg(Cpu *c) {
    uint64_t handle = c->x[0];
    const char *archive = (const char *)(uintptr_t)rd64(c->x[1]);
    const char *label = (const char *)(uintptr_t)rd64(c->x[2]);
    if (archive && label && !strcmp(archive, "LayoutMsg/SystemOption_00")) {
        char buf[1024];
        const char *s = port_text(label, buf, sizeof buf);
        if (s) {
            const Text *t = text_for(label, s);
            if (t) {   // the handle as the game's lookup fills it: the text and its length in UTF-16 units
                wr64(handle, t->text);
                wr32(handle + 8, t->len);
                c->x[0] = 1;
                return;
            }
        }
    }
    c->x[0] = rt_call(c, G_MSG_GET, 4, c->x[0], c->x[1], c->x[2], c->x[3]);
}

// ---- values
static void host_get_bool(Cpu *c) {   // (mgr, bool *out, ctx, key)
    Row *r = row_of_key((uint32_t)c->x[3]);
    if (!r) {
        c->x[0] = rt_call(c, G_GET_BOOL, 4, c->x[0], c->x[1], c->x[2], c->x[3]);
        return;
    }
    *(uint8_t *)(uintptr_t)c->x[1] = (uint8_t)(cfg_bool(r->key) != 0);
    c->x[0] = 1;
}

static void host_set_bool(Cpu *c) {   // (mgr, value, ctx, key)
    Row *r = row_of_key((uint32_t)c->x[3]);
    if (!r) {
        c->x[0] = rt_call(c, G_SET_BOOL, 4, c->x[0], c->x[1], c->x[2], c->x[3]);
        return;
    }
    apply(r, (c->x[1] & 0xff) ? "1" : "0");
    c->x[0] = 1;
}

// The hashes of the ChoiceType names the selection rows compare (the port's choice i is the value i)
static uint32_t g_hash[N_CHOICE_NAMES];
static int g_hashed;
static void hashes(Cpu *c) {
    if (g_hashed) return;
    for (int i = 0; i < N_CHOICE_NAMES; i++) g_hash[i] = (uint32_t)rt_call(c, G_NAME_HASH, 2, rd64(G_CHOICE_NAMES + 8ull * i), 0ull);
    g_hashed = 1;
}

static void host_get_enum(Cpu *c) {   // (mgr, u32 *out, ctx, key)
    Row *r = row_of_key((uint32_t)c->x[3]);
    if (!r) {
        c->x[0] = rt_call(c, G_GET_ENUM, 4, c->x[0], c->x[1], c->x[2], c->x[3]);
        return;
    }
    uint64_t out = c->x[1];   // before hashes(): its calls into the game change the registers
    hashes(c);
    wr32(out, g_hash[choice_now(r)]);
    c->x[0] = 1;
}

static void host_set_enum(Cpu *c) {   // (mgr, hash, ctx, key)
    Row *r = row_of_key((uint32_t)c->x[3]);
    if (!r) {
        c->x[0] = rt_call(c, G_SET_ENUM, 4, c->x[0], c->x[1], c->x[2], c->x[3]);
        return;
    }
    uint32_t want = (uint32_t)c->x[1];
    hashes(c);
    for (int i = 0; i < r->n; i++)
        if (g_hash[i] == want) apply(r, r->vals[i]);
    c->x[0] = 1;
}

__attribute__((constructor)) static void game_menu_register(void) {
    rt_register_host(HOST_LIST, host_list);
    rt_register_host(HOST_MSG, host_msg);
    rt_register_host(HOST_GET_BOOL, host_get_bool);
    rt_register_host(HOST_SET_BOOL, host_set_bool);
    rt_register_host(HOST_GET_ENUM, host_get_enum);
    rt_register_host(HOST_SET_ENUM, host_set_enum);
}
#endif
