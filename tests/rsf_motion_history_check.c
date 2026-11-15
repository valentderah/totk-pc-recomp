// Real history collection under the new menu key, without the legacy DLAA key.
#include <assert.h>
#include "../runtime/vk_mvprev.c"

static int reconstruction_enabled = 1, runtime_available = 1;
int cfg_bool(const char *name) {
    return !strcmp(name, "TOTK_RSF_ENABLED") ? reconstruction_enabled : 0;
}
int vk_dlaa_available(void) { return runtime_available; }
int rt_knob(const char *name, const char *fallback) {
    (void)fallback;
    return !strcmp(name, "DLAA_OBJMV");
}
uint64_t kern_now_ns(void) { return 1; }
void rt_log(const char *format, ...) { (void)format; }
const uint8_t *vk_usnap_bytes(uint64_t address, uint64_t size) {
    (void)address; (void)size;
    return NULL;
}

int main(void) {
    VkExec ex = {0};
    ex.depth.image = (VkImage)1;
    ex.depth.width = 640;
    ex.depth.format = VK_FORMAT_D32_SFLOAT;
    ex.ubo[0][BONES] = (p_bind_buf){.addr = 0x10000, .size = 128};
    ex.ubo[0][PLACE] = (p_bind_buf){.addr = 0x20000, .size = 128};
    uint64_t previous = 0;
    int how;
    vk_mvprev_frame();
    vk_mvprev_draw(&ex);
    vk_mvprev_frame();
    assert(vk_mvprev_find(BONES, 0x10000, 128, &previous, &how));
    assert(previous == 0x10000 && how == MVPREV_SELF);
    assert(vk_mvprev_find(PLACE, 0x20000, 128, &previous, &how));
    vk_mvprev_draw(&ex); // Leave the per-draw deduplication cache populated at shutdown.
    runtime_available = 0;
    vk_mvprev_frame();
    assert(!vk_mvprev_find(BONES, 0x10000, 128, &previous, &how));
    runtime_available = 1;
    vk_mvprev_frame();
    assert(!vk_mvprev_find(PLACE, 0x20000, 128, &previous, &how));
    vk_mvprev_draw(&ex);
    vk_mvprev_frame();
    assert(vk_mvprev_find(BONES, 0x10000, 128, &previous, &how));
    reconstruction_enabled = 0;
    vk_mvprev_frame();
    assert(!vk_mvprev_find(BONES, 0x10000, 128, &previous, &how));
    reconstruction_enabled = 1;
    vk_mvprev_frame();
    vk_mvprev_draw(&ex);
    vk_mvprev_frame();
    assert(vk_mvprev_find(PLACE, 0x20000, 128, &previous, &how));
    free(P.t[0]); free(P.t[1]);
    return 0;
}
