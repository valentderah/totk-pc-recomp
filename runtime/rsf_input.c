#include "rsf_input.h"
#include "cfg.h"
#include "rsf_runtime.h"
#include "platform/platform.h"
#include <rescaleframe/native_latency_vulkan.h>
static void emit(uint32_t kind,uint32_t message,uint32_t flags) {
    if (!rsf_requested()) return;
    __typeof__(&rsf_latencyvk_cpu) sink=rsf_runtime_cpu_sink();
    if (!sink) return; // Input delivery does not load the runtime or create a source.
    rsf_game_cpu_event event={.struct_size=sizeof event,.stage=RSF_GAME_CPU_INPUT_EVENT,
        .session_id=RSF_GAME_SESSION_ID,.source_frame_id=0,.timestamp_qpc=plat_counter(),.qpc_frequency=plat_counter_hz(),
        .input_kind=kind,.message_id=message,.event_version=RSF_GAME_CPU_EVENT_VERSION,.flags=flags};
    sink(&event); // Source-zero facts are associated only by the core's bounded FRAME_BEGIN rules.
}
void rsf_input_received(uint32_t kind) {
    if (!kind || (kind & ~(RSF_INPUT_KEYBOARD|RSF_INPUT_MOUSE|RSF_INPUT_CONTROLLER))) return;
    emit(kind,0,RSF_GAME_CPU_INPUT_UNFRAMED);
}
void rsf_input_window_message(uint32_t message) {
    emit(0,message,RSF_GAME_CPU_WINDOW_MESSAGE|RSF_GAME_CPU_INPUT_UNFRAMED);
}
enum {
    WIN_KEYDOWN = 0x0100, WIN_KEYUP = 0x0101, WIN_SYSKEYDOWN = 0x0104, WIN_SYSKEYUP = 0x0105,
    WIN_MOUSE_FIRST = 0x0200, WIN_MOUSE_LAST = 0x020e
};
void rsf_input_win_dequeued(uint32_t message) {
    rsf_input_window_message(message);
    if (message >= WIN_MOUSE_FIRST && message <= WIN_MOUSE_LAST) {
        rsf_input_received(RSF_INPUT_MOUSE);
        return;
    }
    // Win32 queue IDs, without WPARAM/LPARAM or key/button data. WM_INPUT is
    // classified only after GetRawInputData verifies its actual packet type.
    switch (message) {
    case WIN_KEYDOWN: case WIN_KEYUP: case WIN_SYSKEYDOWN: case WIN_SYSKEYUP: // WM_[SYS]KEYDOWN/UP
        rsf_input_received(RSF_INPUT_KEYBOARD); break;
    }
}
