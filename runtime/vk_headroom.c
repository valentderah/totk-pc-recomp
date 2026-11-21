// VRAM headroom: the next VMA block of a memory type is allocated on a background thread, not on the render thread.
//
// VMA places an allocation in an existing block of its memory type and allocates a new block (vkAllocateMemory, 32 to
// 256 MiB) only when none has room. On the render thread that took 8-19 ms per block, several times in the first
// minute of play while VRAM grows (textures from create_tex, mirrors). A new VkDeviceMemory made by any other thread
// (VMA's pfnAllocate callback) wakes this thread for its memory type, which it then checks every 500 ms. A check
// allocates half a default block of that type (the largest size VMA still places in blocks, not in dedicated memory).
// When a block has that much room the allocation lands there and is freed at once. When none has, VMA allocates a new
// block here, and the allocation stays as that block's anchor: VMA deletes an empty block at the next free of its
// type (VmaBlockVector::Free, "we have another empty block"), so a spare block left empty lasted until the next
// texture went. The previous anchor is freed then, which gives its half block to the render thread, so at most half
// a block per memory type is held back.
// Under VRAM pressure no headroom is kept: the spare block counts against the budget that decides whether a pool gets
// its VRAM mirror (vk_memory.c), so while use plus two blocks passes 85% of the budget the anchor goes and nothing new
// is allocated. The same holds when the system runs short of memory (room), and for HR_HOLD_MS after an allocation
// failed anywhere (vk_headroom_release).
// The texture pool (vk_memory.c vk_texture_image) is kept ahead the same way, with its own anchor of half its block.
// TOTK_VRAM_HEADROOM=0 turns it off.

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "platform/platform.h"

#define HR_PERIOD_MS 500
#define HR_SMALL_HEAP (1ull << 30)   // VMA_SMALL_HEAP_MAX_SIZE: smaller heaps use blocks of a heap eighth
#define HR_HOLD_MS 30000u

static struct {
    int on, stop;
    PlatMutex lock;             // held around each top-up; vk_headroom_stop takes it before the allocator goes
    int started;
    PlatEvent ev;               // auto-reset: another thread allocated, or stop
    volatile uint32_t tid;      // the headroom thread's id
    volatile int64_t types;    // memory types a thread other than this one allocated device memory of
    volatile int64_t made;     // blocks this thread brought in
    volatile int64_t other;    // device memory of 32 MiB or more other threads allocated (VMA blocks, large dedicated)
    VmaAllocation anchor[64]; // per memory type: the half block that keeps the newest spare block from being deleted
    VmaPool pool;             // the texture pool, its memory type and block size, and its anchor
    uint32_t pool_type;
    VkDeviceSize pool_block;
    VmaAllocation pool_anchor;
    volatile uint64_t hold_until;   // plat_now_ms before which no spare block is kept (vk_headroom_release)
} H;

static void VKAPI_PTR on_allocate(VmaAllocator a, uint32_t type, VkDeviceMemory m, VkDeviceSize size, void *user) {
    (void)a, (void)m, (void)size, (void)user;
    if (!H.on || type >= 64) return;
    if (plat_thread_id() == H.tid) {
        plat_inc64(&H.made);
        return;
    }
    if (size >= 32ull << 20) plat_inc64(&H.other);
    plat_or64(&H.types, (int64_t)(1ull << type));
    plat_event_set(&H.ev);
}

const VmaDeviceMemoryCallbacks *vk_headroom_callbacks(void) {
    static const VmaDeviceMemoryCallbacks cb = {.pfnAllocate = on_allocate};
    H.on = !cfg_get("TOTK_VRAM_HEADROOM") || cfg_bool("TOTK_VRAM_HEADROOM");
    return H.on ? &cb : NULL;
}

static VkDeviceSize block_size(const VkCtx *c, uint32_t type) {
    VkDeviceSize heap = c->mem_props.memoryHeaps[c->mem_props.memoryTypes[type].heapIndex].size;
    return heap <= HR_SMALL_HEAP ? heap / 8 : c->vma_block;   // VMA's preferredLargeHeapBlockSize
}

// The spare block is memory the game does not use yet; where device memory takes host memory it is kept only while the
// system has four blocks to spare beyond the reserve (vk_mem_host_room). On the RX 9070 the spare blocks held 1.4 GB
// of commit.
static int room(const VkCtx *c, uint32_t type, VkDeviceSize block) {
    VmaBudget b[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, b);
    const VmaBudget *h = &b[c->mem_props.memoryTypes[type].heapIndex];
    return h->budget && h->usage + 2 * block <= h->budget / 100 * 85 && vk_mem_host_room() >= 4 * block;
}

static void drop_anchor(const VkCtx *c, VmaAllocation *anchor) {
    if (*anchor) vmaFreeMemory(c->vma, *anchor);
    *anchor = NULL;
}

// pool NULL: the default pool of the type
static void top_up(const VkCtx *c, uint32_t type, VmaPool pool, VkDeviceSize block, VmaAllocation *anchor) {
    // Device-local types only: textures and mirrors grow there. Host memory types are woken by the staging pool's own
    // blocks (vk_memory.c vk_staging), while their default pool held 4 MiB: on the RX 9070 the spare blocks of types 1
    // and 3 held 512 MiB of commit for nothing.
    if (!(c->mem_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
        plat_now_ms() < __atomic_load_n(&H.hold_until, __ATOMIC_RELAXED) || !room(c, type, block)) {
        drop_anchor(c, anchor);
        return;
    }
    VkMemoryRequirements req = {.size = block / 2, .alignment = 256, .memoryTypeBits = 1u << type};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_UNKNOWN, .pool = pool};
    VmaAllocation a;
    int64_t before = H.made;
    if (vmaAllocateMemory(c->vma, &req, &ai, &a, NULL) != VK_SUCCESS) return;
    if (H.made == before) {   // there was room
        vmaFreeMemory(c->vma, a);
        return;
    }
    drop_anchor(c, anchor);
    *anchor = a;
    static volatile int32_t logged;
    if (plat_inc32(&logged) <= 32)
        rt_log("vk: headroom: a %.0f MiB block of memory type %u%s allocated ahead (%lld so far; %lld allocations of "
               "32 MiB or more on other threads)", (double)block / 1048576.0, type, pool ? " (texture pool)" : "",
               (long long)H.made, (long long)H.other);
}

static void headroom_main(void *arg) {
    (void)arg;
    __atomic_store_n(&H.tid, plat_thread_id(), __ATOMIC_RELEASE);
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    uint64_t watch = 0;   // memory types seen so far
    for (;;) {
        if (watch) plat_event_wait_ns(&H.ev, (uint64_t)HR_PERIOD_MS * 1000000u);
        else plat_event_wait(&H.ev);
        watch |= (uint64_t)plat_xchg64(&H.types, 0);
        plat_mutex_lock(&H.lock);
        const VkCtx *c = H.stop ? NULL : vk_ctx();
        for (uint32_t t = 0; c && t < c->mem_props.memoryTypeCount && t < 64; t++)
            if (watch >> t & 1) {
                top_up(c, t, NULL, block_size(c, t), &H.anchor[t]);
                if (H.pool && t == H.pool_type) top_up(c, t, H.pool, H.pool_block, &H.pool_anchor);
            }
        int stop = H.stop;
        if (stop && (c = vk_ctx())) {
            for (uint32_t t = 0; t < 64; t++) drop_anchor(c, &H.anchor[t]);
            drop_anchor(c, &H.pool_anchor);
        }
        plat_mutex_unlock(&H.lock);
        if (stop) return;
    }
}

// Before the allocator is destroyed: waits for a running top-up, frees the anchors, and the thread allocates no more.
void vk_headroom_stop(void) {
    if (!H.started) return;
    plat_mutex_lock(&H.lock);
    H.stop = 1;
    const VkCtx *c = vk_ctx();
    for (uint32_t t = 0; c && t < 64; t++) drop_anchor(c, &H.anchor[t]);
    if (c) drop_anchor(c, &H.pool_anchor);
    plat_mutex_unlock(&H.lock);
    plat_event_set(&H.ev);
}

void vk_headroom_pool(VmaPool pool, uint32_t type, VkDeviceSize block) {
    if (!H.started) return;
    plat_mutex_lock(&H.lock);
    const VkCtx *c = vk_ctx();
    if (c) drop_anchor(c, &H.pool_anchor);
    H.pool = pool, H.pool_type = type, H.pool_block = block;
    plat_mutex_unlock(&H.lock);
}

// After a failed allocation: the spare blocks go at once (an empty VMA block takes the next allocation of its type or
// is given back), and none is kept for HR_HOLD_MS. H.lock is held only around VMA calls, so the caller may hold any lock
// of ours, just no VMA lock.
void vk_headroom_release(void) {
    __atomic_store_n(&H.hold_until, plat_now_ms() + HR_HOLD_MS, __ATOMIC_RELAXED);
    if (!H.started) return;
    int n = 0;
    plat_mutex_lock(&H.lock);
    const VkCtx *c = H.stop ? NULL : vk_ctx();
    for (uint32_t t = 0; c && t < 64; t++)
        if (H.anchor[t]) drop_anchor(c, &H.anchor[t]), n++;
    if (c && H.pool_anchor) drop_anchor(c, &H.pool_anchor), n++;
    plat_mutex_unlock(&H.lock);
    static volatile int32_t logged;
    if (n && plat_inc32(&logged) <= 8) rt_log("vk: headroom: %d spare blocks freed after a failed allocation", n);
}

void vk_headroom_start(void) {
    if (!H.on || H.started) return;
    plat_mutex_init(&H.lock);
    plat_event_init(&H.ev, 0, 0);
    if (!plat_thread_spawn(headroom_main, NULL, "vk headroom", 0)) {
        H.on = 0;
        rt_log("vk: headroom: no thread, new VMA blocks stay on the allocating thread");
        return;
    }
    H.started = 1;
    rt_log("vk: headroom: the next VMA block of each memory type in use is allocated on a background thread "
           "(TOTK_VRAM_HEADROOM=0 turns it off)");
}
