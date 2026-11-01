// --probe-window: presents 300 frames of a cycling clear colour through vk_present_image, reads the swapchain image back
// and compares every pixel, measures the vsync rate, flips the focus state and drives the cursor release with synthetic
// window events (the window back end's wsys_probe_*, gfx_wsys.h) and resizes the window. Run with TOTK_VK_VALIDATION=1
// to count validation errors.
#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "gfx_wsys.h"
#include "input.h"
#include "ui/menu.h"
#include "vk.h"
#include "platform/platform.h"

#define SRC_W 640
#define SRC_H 360
#define FRAMES 300

typedef struct {
    VkImage img;
    VmaAllocation alloc;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
} Source;

static int fail(const char *what) {
    printf("probe-window: FAILED (%s)\n", what);
    return 1;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 s0,
                    VkAccessFlags2 a0, VkPipelineStageFlags2 s1, VkAccessFlags2 a1) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = s0, .srcAccessMask = a0,
                               .dstStageMask = s1, .dstAccessMask = a1, .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

static int source_create(Source *s) {
    VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {SRC_W, SRC_H, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &s->img, &s->alloc, NULL) != VK_SUCCESS) return 0;
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = c->gfx_family};
    if (vkCreateCommandPool(c->device, &pi, NULL, &s->pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo ba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s->pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vkAllocateCommandBuffers(c->device, &ba, &s->cb) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    return vkCreateFence(c->device, &fi, NULL, &s->fence) == VK_SUCCESS;
}

static void source_destroy(Source *s) {
    VkCtx *c = vk_ctx();
    if (s->fence) vkDestroyFence(c->device, s->fence, NULL);
    if (s->pool) vkDestroyCommandPool(c->device, s->pool, NULL);
    if (s->img) vmaDestroyImage(c->vma, s->img, s->alloc);
}

// Clears the source to rgba and leaves it in TRANSFER_SRC_OPTIMAL; the GPU work is waited for here, which is stricter
// than the vk_present_image contract needs.
static int source_clear(Source *s, int first, const float rgba[4]) {
    VkCtx *c = vk_ctx();
    vkResetCommandBuffer(s->cb, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(s->cb, &bi);
    barrier(s->cb, s->img, first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, first ? 0 : VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearColorValue cv;
    memcpy(cv.float32, rgba, sizeof cv.float32);
    VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(s->cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rng);
    barrier(s->cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);
    vkEndCommandBuffer(s->cb);
    VkCommandBufferSubmitInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi};
    vk_queue_lock();
    VkResult r = vkQueueSubmit2(c->gfx_queue, 1, &si, s->fence);
    vk_queue_unlock();
    if (r != VK_SUCCESS) return 0;
    r = vkWaitForFences(c->device, 1, &s->fence, VK_TRUE, 5000000000ull);
    vkResetFences(c->device, 1, &s->fence);
    return r == VK_SUCCESS;
}

static uint8_t to_byte(float f) { return (uint8_t)(f * 255.0f + 0.5f); }

// Presents one colour and checks every pixel of the readback. Returns 0 when it matches.
static int present_and_check(Source *s, int first, int i, int *w_out, int *h_out) {
    float rgba[4] = {(float)((i * 7) & 255) / 255.0f, (float)((i * 13) & 255) / 255.0f, (float)((i * 29) & 255) / 255.0f, 1.0f};
    if (!source_clear(s, first, rgba)) return 1;
    vk_present_image(s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_FORMAT_R8G8B8A8_UNORM, SRC_W, SRC_H, NULL);
    const uint8_t *px;
    int w, h;
    VkFormat fmt;
    if (!vk_present_captured(&px, &w, &h, &fmt)) { printf("probe-window: frame %d was not presented\n", i); return 2; }
    uint8_t rgb[3] = {to_byte(rgba[0]), to_byte(rgba[1]), to_byte(rgba[2])};
    uint8_t want[4];
    if (fmt == VK_FORMAT_B8G8R8A8_UNORM) { want[0] = rgb[2]; want[1] = rgb[1]; want[2] = rgb[0]; }
    else if (fmt == VK_FORMAT_R8G8B8A8_UNORM) { want[0] = rgb[0]; want[1] = rgb[1]; want[2] = rgb[2]; }
    else { printf("probe-window: swapchain format %d is not UNORM RGBA/BGRA\n", (int)fmt); return 3; }
    want[3] = 255;
    uint32_t wv, black = 0xff000000u;   // opaque black in both byte orders
    memcpy(&wv, want, 4);
    int r[4];   // the picture's rectangle; bars of black around it when the window's shape differs
    gfx_fit_rect(SRC_W, SRC_H, w, h, r);
    const uint32_t *p = (const uint32_t *)px;
    for (long k = 0, n = (long)w * h; k < n; k++) {
        int x = (int)(k % w), y = (int)(k / w);
        uint32_t e = x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3] ? wv : black;
        if (p[k] != e) {
            printf("probe-window: frame %d pixel %ld (%d, %d) is %08x, expected %08x\n", i, k, x, y, p[k], e);
            return 4;
        }
    }
    *w_out = w;
    *h_out = h;
    return 0;
}

int probe_window(void) {
    if (!menu_font_check()) return fail("the menu's font check takes a damaged font or refuses a good one");
    gfx_window_start();
    VkCtx *c = vk_ctx();
    if (!c) return fail("no Vulkan device");
    vk_present_capture(1);
    Source s = {0};
    if (!source_create(&s)) return fail("source image");

    int w = 0, h = 0;
    for (int i = 0; i < FRAMES; i++)
        if (present_and_check(&s, i == 0, i, &w, &h)) return fail("presented pixels differ");
    printf("probe-window: %d presents, readback %dx%d exact\n", FRAMES, w, h);

    // Focus: synthetic focus loss and gain, delivered synchronously on the window thread.
    unsigned m0 = gfx_window_focus_messages();
    wsys_probe_focus(0);
    int off = gfx_window_focused();
    wsys_probe_focus(1);
    int on = gfx_window_focused();
    unsigned m1 = gfx_window_focus_messages();
    if (off != 0 || on != 1 || m1 - m0 != 2) {
        printf("probe-window: focus off=%d on=%d messages=%u\n", off, on, m1 - m0);
        return fail("focus");
    }
    printf("probe-window: focus off/on pushed 2 messages (15)\n");

    // Cursor release: Esc releases, a client click recaptures, a title bar activation releases (where the window system
    // reports one). The probe window may not be the foreground window, so this checks the release flag, not the capture.
    wsys_probe_escape();
    int esc = input_cursor_released();
    wsys_probe_click(0);
    int click = input_cursor_released();
    wsys_probe_click(1);
    int client = input_cursor_released();
    int caption = wsys_probe_click(2) ? input_cursor_released() : 1;
    wsys_probe_focus(0);
    wsys_probe_focus(1);
    int back = input_cursor_released();
    if (menu_is_open()) menu_toggle();   // Esc opened the menu: the presents below compare the bare game image
    if (!esc || click || client || !caption || back) {
        printf("probe-window: released esc=%d click=%d client=%d caption=%d reactivated=%d\n", esc, click, client, caption, back);
        return fail("cursor release");
    }
    printf("probe-window: cursor released by Esc and title bar activation, recaptured by a click\n");

    // Resize: the client area changes, the swapchain follows, presenting still reads back exact.
    gfx_window_resize(1280, 720);
    int cw = 0, ch = 0;
    wsys_client_size(&cw, &ch);
    for (int i = 0; i < 10; i++)
        if (present_and_check(&s, 0, FRAMES + i, &w, &h)) return fail("presents after resize");
    if (w != cw || h != ch) {
        printf("probe-window: readback %dx%d, client area %dx%d\n", w, h, cw, ch);
        return fail("swapchain size after resize");
    }
    printf("probe-window: resized to %dx%d, presents still exact\n", w, h);
    vk_present_capture(0);

    // Vsync: count ticks between two tick edges about 5 s apart.
    static volatile int32_t ticks;
    gfx_vsync_bind_counter(&ticks);
    long a = ticks;
    while (ticks == a) plat_sleep_ms(1);
    uint64_t t0 = plat_now_ns();
    long n0 = ticks;
    plat_sleep_ms(5000);
    long b = ticks;
    while (ticks == b) plat_sleep_ms(1);
    uint64_t t1 = plat_now_ns();
    long n1 = ticks;
    double rate = (double)(n1 - n0) * 1e9 / (double)(t1 - t0);
    printf("probe-window: vsync %ld ticks in %.3f s = %.3f Hz\n", n1 - n0, (double)(t1 - t0) / 1e9, rate);
    if (rate < 60.0 * 0.995 || rate > 60.0 * 1.005) return fail("vsync rate");

    source_destroy(&s);
    vk_shutdown();
    long errors = vk_validation_errors_total();
    if (errors) {
        printf("probe-window: FAILED (%ld validation errors)\n", errors);
        return 1;
    }
    printf("probe-window: passed (%d frames, %ld validation errors, vsync %.1f Hz)\n", FRAMES, errors, rate);
    return 0;
}
