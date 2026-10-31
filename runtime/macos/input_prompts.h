#ifndef TOTK_NATIVE_INPUT_PROMPTS_H
#define TOTK_NATIVE_INPUT_PROMPTS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    NATIVE_PROMPT_KEYBOARD, NATIVE_PROMPT_XBOX, NATIVE_PROMPT_PLAYSTATION,
    NATIVE_PROMPT_SWITCH, NATIVE_PROMPT_GENERIC,
} NativePromptDevice;
typedef enum {
    NATIVE_GLYPH_KEY, NATIVE_GLYPH_MOUSE, NATIVE_GLYPH_MOUSE_MOVE,
    NATIVE_GLYPH_CIRCLE, NATIVE_GLYPH_CROSS, NATIVE_GLYPH_TRIANGLE,
    NATIVE_GLYPH_SQUARE, NATIVE_GLYPH_SHOULDER, NATIVE_GLYPH_STICK,
    NATIVE_GLYPH_DPAD, NATIVE_GLYPH_KEYBOARD, NATIVE_GLYPH_GAMEPAD, NATIVE_GLYPH_KEY_GROUP,
} NativePromptKind;
typedef struct {
    NativePromptKind kind;
    const char *label;
    unsigned detail;
} NativePromptGlyph;
typedef struct {
    uint64_t revision;
    NativePromptDevice device;
    bool swap_faces;
    uint16_t buttons[17];
} NativePromptSnapshot;

// Device changes are published by the input owners; the font thread takes one
// coherent snapshot for an entire atlas update. Idle analog noise is not input.
uint64_t native_prompts_revision(void);
void native_prompts_use(NativePromptDevice device, bool swap_faces);
void native_prompts_controller_map(const uint16_t buttons[17]);
NativePromptSnapshot native_prompts_snapshot(void);
NativePromptDevice native_prompts_identify(const char *category, const char *vendor);
bool native_prompt_glyph(uint32_t codepoint, const NativePromptSnapshot *, NativePromptGlyph *);
bool native_prompt_raster(const NativePromptGlyph *, uint8_t *pixels, unsigned width, unsigned height);
bool native_prompt_diagram(const NativePromptSnapshot *, uint8_t *rgba, unsigned width, unsigned height);
#endif
