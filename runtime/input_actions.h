// Independently bound gameplay actions; the verified game adapter supplies context and virtual action masks.
#pragma once
#include <stdint.h>
#include "input_map.h"

enum {
#define ACTION(name,label,key1,key2,pad) IA_##name,
#include "input_actions.def"
#undef ACTION
    IA_COUNT, IA_VIRTUAL_COUNT = 9
};
enum { IC_FOOT, IC_CLIMB, IC_SWIM, IC_AIR, IC_GLIDE, IC_BOW, IC_RIDING, IC_ULTRAHAND, IC_ULTRAHAND_HOLD, IC_FUSE, IC_ASCEND, IC_RECALL, IC_AUTOBUILD, IC_CAMERA, IC_AMIIBO, IC_SCOPE, IC_SURF, IC_ROBOT, IC_SLED, IC_MENU, IC_DIALOGUE, IC_COUNT };
// Common controls stay shared. Advanced adds overrides for explicitly distinct actions.
enum { INPUT_ACTIONS_STANDARD = -1 };
typedef struct { int active; uint64_t generation; int kind; int lockout; } InputActionContext;
typedef struct { uint32_t held, pressed, released; int active; int kind; int advanced; int lockout; } InputActionFrame;
typedef struct {
    InputActionContext (*context)(void);
    int (*interact_available)(void);  // 1 prompt, 0 no prompt, -1 unknown
    int (*scripted)(void);  // optional bounded test provider; permits scripted polling without window focus
    void (*devices)(uint8_t keys[KEY_COUNT], uint8_t pads[PAD_COUNT]);  // optional additional input provider
} InputActionBackend;
void input_actions_register(const InputActionBackend *backend);
int input_actions_supported(void);
int input_actions_scripted(void);
const char *input_actions_name(int action);
const char *input_actions_label(int action);
int input_actions_advanced(void);
int input_actions_split_guard_context(int context);
int input_actions_native_context(int context);
int input_actions_exclusive_target(void);
const char *input_actions_context_name(int context);
const char *input_actions_context_label(int context);
const char *input_actions_context_action(int context, int action);
// Returns the owning binding set; -1 means the shared control.
int input_actions_binding_context(int context, int action);
void input_actions_config_key(int context, int action, char *out, unsigned capacity);
void input_actions_map_for(int context, InputMap *out);
void input_actions_save_for(int context, const InputMap *map);
void input_actions_initialize_advanced(void);
void input_actions_set_native_map(const InputMap *map);
void input_actions_sticks(int32_t axes[4]);
uint64_t input_actions_owned_buttons(void);
void input_actions_map(InputMap *out);
void input_actions_save(const InputMap *map);
int input_actions_set_key(InputMap *map, int action, int slot, int key);
int input_actions_set_pad(InputMap *map, int action, int layout, int pad);
void input_actions_reset(void);
// Called on the ordinary input poll with the physical devices. Claimed inputs are removed only in supported gameplay;
// the return is the broad Interact/Bow/Throw/Guard input, which uses the game's complete native button behavior.
uint64_t input_actions_sample(uint8_t keys[KEY_COUNT], uint8_t pads[PAD_COUNT], int layout, int allowed);
int input_actions_routing(void);
InputActionFrame input_actions_frame(int64_t clock);
