#ifndef TOTK_NATIVE_INPUT_BINDINGS_H
#define TOTK_NATIVE_INPUT_BINDINGS_H
#include <stdint.h>

// Mac virtual key codes are physical positions, independent of the text layout.
// Input and in-game prompts share this table; the first binding is the prompt.
typedef struct { unsigned key; uint16_t button; const char *label; } NativeKeyBinding;
enum { NATIVE_KEY_INTERACT = 14, NATIVE_KEY_MENU = 48, NATIVE_KEY_MAP = 46 };
static const NativeKeyBinding native_key_bindings[] = {
    {NATIVE_KEY_INTERACT, 1, "E"}, {36, 1, "Enter"},
    {56, 2, "Shift"}, {60, 2, "Shift"}, {53, 2, "Esc"}, {49, 4, "Space"},
    {8, 16, "C"}, {9, 32, "V"}, {12, 64, "Q"}, {3, 128, "F"},
    {59, 256, "Ctrl"}, {62, 256, "Ctrl"}, {15, 512, "R"},
    {NATIVE_KEY_MENU, 1024, "Tab"}, {NATIVE_KEY_MAP, 2048, "M"},
    {18, 4096, "1"}, {19, 8192, "2"}, {20, 16384, "3"}, {21, 32768, "4"},
};
static inline uint16_t native_key_button(unsigned code) {
    for (unsigned i = 0; i < sizeof native_key_bindings / sizeof *native_key_bindings; i++)
        if (native_key_bindings[i].key == code) return native_key_bindings[i].button;
    return 0;
}
static inline const char *native_key_label(uint16_t button) {
    for (unsigned i = 0; i < sizeof native_key_bindings / sizeof *native_key_bindings; i++)
        if (native_key_bindings[i].button == button) return native_key_bindings[i].label;
    return "-";
}
static inline uint16_t native_mouse_button_mapping(unsigned index) {
    static const uint16_t buttons[] = {8, 256, 512, 1, 2};
    return index < sizeof buttons / sizeof *buttons ? buttons[index] : 0;
}
#endif
