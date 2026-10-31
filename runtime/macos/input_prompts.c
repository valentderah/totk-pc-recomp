#include "input_prompts.h"
#include "input_bindings.h"
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

static pthread_mutex_t prompt_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t published_revision = 1;
static NativePromptSnapshot current = {
    .revision = 1, .device = NATIVE_PROMPT_KEYBOARD,
    .buttons = {2,1,8,4,16,32,64,128,256,512,1024,2048,4096,8192,16384,32768,2048},
};
uint64_t native_prompts_revision(void) {
    return atomic_load_explicit(&published_revision, memory_order_acquire);
}
static void publish(void) {
    current.revision++;
    atomic_store_explicit(&published_revision, current.revision, memory_order_release);
}
void native_prompts_use(NativePromptDevice device, bool swap_faces) {
    if ((unsigned)device > NATIVE_PROMPT_GENERIC) return;
    if (device == NATIVE_PROMPT_KEYBOARD) swap_faces = false;
    pthread_mutex_lock(&prompt_lock);
    if (device != current.device || swap_faces != current.swap_faces) {
        current.device = device;
        current.swap_faces = swap_faces;
        publish();
    }
    pthread_mutex_unlock(&prompt_lock);
}
void native_prompts_controller_map(const uint16_t buttons[17]) {
    pthread_mutex_lock(&prompt_lock);
    if (memcmp(current.buttons, buttons, sizeof current.buttons)) {
        memcpy(current.buttons, buttons, sizeof current.buttons);
        publish();
    }
    pthread_mutex_unlock(&prompt_lock);
}
NativePromptSnapshot native_prompts_snapshot(void) {
    pthread_mutex_lock(&prompt_lock);
    NativePromptSnapshot out = current;
    pthread_mutex_unlock(&prompt_lock);
    return out;
}
static bool contains(const char *s, const char *word) {
    if (!s) return false;
    for (; *s; s++) {
        unsigned n = 0;
        while (word[n] && s[n] && tolower((unsigned char)s[n]) == word[n]) n++;
        if (!word[n]) return true;
    }
    return false;
}
NativePromptDevice native_prompts_identify(const char *category, const char *vendor) {
    const char *s[] = {category, vendor};
    for (unsigned i = 0; i < 2; i++) {
        if (contains(s[i], "nintendo") || contains(s[i], "switch") || contains(s[i], "joy-con") || contains(s[i], "joycon")) return NATIVE_PROMPT_SWITCH;
        if (contains(s[i], "playstation") || contains(s[i], "dualsense") || contains(s[i], "dualshock")) return NATIVE_PROMPT_PLAYSTATION;
        if (contains(s[i], "xbox")) return NATIVE_PROMPT_XBOX;
    }
    return NATIVE_PROMPT_GENERIC;
}
static bool button_glyph(uint16_t button, const NativePromptSnapshot *s, NativePromptGlyph *out) {
    if (s->device == NATIVE_PROMPT_KEYBOARD) {
        // Mouse side buttons remain optional aliases: an ordinary two-button
        // mouse must still show a usable interact/cancel binding.
        if (button == 8 || button == 256) {
            *out = (NativePromptGlyph){NATIVE_GLYPH_MOUSE, "", button == 8 ? 0 : 1};
        } else *out = (NativePromptGlyph){NATIVE_GLYPH_KEY, native_key_label(button), 0};
        return true;
    }
    static const uint16_t defaults[] = {2,1,8,4,16,32,64,128,256,512,1024,2048,4096,8192,16384,32768,2048};
    int index = -1;
    // Prefer the usual position when there are duplicate aliases (e.g. sprint
    // on both the south button and L3), otherwise invert the actual map.
    for (unsigned i = 0; i < 17; i++) if ((s->buttons[i] & button) && defaults[i] == button) { index = (int)i; break; }
    if (index < 0) for (unsigned i = 0; i < 17; i++) if (s->buttons[i] & button) { index = (int)i; break; }
    if (index < 0) { *out = (NativePromptGlyph){NATIVE_GLYPH_KEY, "-", 0}; return true; }
    if (index < 4 && s->swap_faces) index ^= 1;
    if (index < 4) {
        static const char *xbox[] = {"A","B","X","Y"};
        static const char *nintendo[] = {"B","A","Y","X"};
        if (s->device == NATIVE_PROMPT_PLAYSTATION) {
            const NativePromptKind shapes[] = {NATIVE_GLYPH_CROSS,NATIVE_GLYPH_CIRCLE,NATIVE_GLYPH_SQUARE,NATIVE_GLYPH_TRIANGLE};
            *out = (NativePromptGlyph){shapes[index], "", 0};
        } else *out = (NativePromptGlyph){NATIVE_GLYPH_CIRCLE, s->device == NATIVE_PROMPT_SWITCH ? nintendo[index] : xbox[index], 0};
    } else if (index < 6) {
        static const char *names[] = {"LS","RS","L3","R3","L","R"};
        unsigned offset = s->device == NATIVE_PROMPT_PLAYSTATION ? 2 : s->device == NATIVE_PROMPT_SWITCH ? 4 : 0;
        *out = (NativePromptGlyph){NATIVE_GLYPH_STICK, names[index - 4 + offset], 16};
    } else if (index < 10) {
        static const char *names[3][4] = {{"LB","RB","LT","RT"},{"L1","R1","L2","R2"},{"L","R","ZL","ZR"}};
        unsigned row = s->device == NATIVE_PROMPT_PLAYSTATION ? 1 : s->device == NATIVE_PROMPT_SWITCH ? 2 : 0;
        *out = (NativePromptGlyph){NATIVE_GLYPH_SHOULDER,names[row][index-6],0};
    } else if (index == 10 || index == 11 || index == 16) {
        const char *label = index == 16 ? "Touch" : index == 10 ? "Menu" : "View";
        if (s->device == NATIVE_PROMPT_PLAYSTATION && index != 16) label = index == 10 ? "Options" : "Share";
        if (s->device == NATIVE_PROMPT_SWITCH && index != 16) label = index == 10 ? "+" : "-";
        *out = (NativePromptGlyph){NATIVE_GLYPH_SHOULDER,label,0};
    } else {
        // Direction bits: left, up, right, down.
        *out = (NativePromptGlyph){NATIVE_GLYPH_DPAD,"",1u << (index-12)};
    }
    return true;
}
bool native_prompt_glyph(uint32_t c, const NativePromptSnapshot *s, NativePromptGlyph *out) {
    if (!s || !out) return false;
    static const uint16_t face[] = {1,2,4,8,64,128,256,512};
    if (c >= 0xe040 && c <= 0xe047) return button_glyph(face[c-0xe040],s,out);
    if (c == 0xe04b || c == 0xe04c) return button_glyph(c == 0xe04b ? 1024 : 2048,s,out);
    if (c == 0xe056 || c == 0xe057) return button_glyph(c == 0xe056 ? 16 : 32,s,out);
    if (c >= 0xe060 && c <= 0xe063) {
        static const uint16_t buttons[] = {8192,32768,4096,16384};
        return button_glyph(buttons[c-0xe060],s,out);
    }
    if (c == 0xe049 || c == 0xe064 || c == 0xe065) {
        if (s->device == NATIVE_PROMPT_KEYBOARD) *out = (NativePromptGlyph){NATIVE_GLYPH_KEY_GROUP,"",c == 0xe064 ? 0xa000 : c == 0xe065 ? 0x5000 : 0xf000};
        else *out = (NativePromptGlyph){NATIVE_GLYPH_DPAD,"",c == 0xe064 ? 10 : c == 0xe065 ? 5 : 15};
        return true;
    }
    if ((c >= 0xe050 && c <= 0xe05f) || c == 0xe068) {
        bool right = (c & 1) || c == 0xe068;
        unsigned pair = (c-0xe050)/2;
        static const unsigned directions[] = {8,2,5,0,4,1,10,15};
        unsigned direction = c == 0xe068 ? 15 : directions[pair];
        if (s->device == NATIVE_PROMPT_KEYBOARD) {
            static const char *labels[] = {"S","W","A D","","D","A","W S","WASD"};
            *out = (NativePromptGlyph){right ? NATIVE_GLYPH_MOUSE_MOVE : NATIVE_GLYPH_KEY, right ? "" : labels[pair], direction};
        } else *out = (NativePromptGlyph){NATIVE_GLYPH_STICK,right ? "R" : "L",direction};
        return true;
    }
    if (c == 0xe066) {
        *out = (NativePromptGlyph){s->device == NATIVE_PROMPT_KEYBOARD ? NATIVE_GLYPH_KEYBOARD : NATIVE_GLYPH_GAMEPAD,"",0};
        return true;
    }
    return false;
}
