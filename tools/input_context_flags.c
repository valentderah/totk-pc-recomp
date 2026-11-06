// Read-only research observer for a verified pause-flag registry contract. Game addresses/hashes are supplied locally.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
extern int plat_mem_read(uint64_t, void *, size_t);
extern int64_t frame_step_world_us(void);
extern void input_context_observe(uint64_t, uint32_t, int);
extern void input_context_word_observe(uint64_t, unsigned, uint32_t);
extern void input_context_entry_observe(uint64_t, uint32_t, uint8_t);
static pthread_once_t once = PTHREAD_ONCE_INIT;
static uint64_t global;
static uint32_t hashes[8];
static uint32_t block_hash;
static int block_enabled;
static _Atomic uint64_t activation;
static unsigned count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t previous = -1;
static uint64_t previous_manager, previous_table;
extern const char *input_context_default_contract(void) __attribute__((weak));
extern const char *input_context_default_block(void) __attribute__((weak));

static void init_block(void) {
    const char *s = getenv("TOTK_CONTEXT_BLOCK_HASH");
    if(!s && input_context_default_block)s=input_context_default_block();
    if (!s || !*s) return;
    errno = 0; char *end;
    uint64_t value = strtoull(s, &end, 0);
    if (*s < '0' || *s > '9' || errno || *end || value > UINT32_MAX) return;
    for (unsigned i = 0; i < count; i++) if (hashes[i] == value) {
        block_hash = (uint32_t)value; block_enabled = 1;
        atomic_store(&activation, 1ull << 63);
        return;
    }
}

static void init(void) {
    const char *s = getenv("TOTK_CONTEXT_FLAGS");
    if(!s && input_context_default_contract)s=input_context_default_contract();
    if (!s || !*s) return;
    unsigned n = 0;
    while (*s && n < 9) {
        if (*s < '0' || *s > '9') goto invalid;
        errno = 0; char *end;
        uint64_t v = strtoull(s, &end, 0);
        if (errno || end == s || (*end && *end != ':')) goto invalid;
        if (!n) global = v;
        else { if (v > UINT32_MAX) goto invalid; hashes[n - 1] = (uint32_t)v; }
        n++;
        if (!*end) { if (n < 2 || !global || global % 8) goto invalid; count = n - 1; init_block(); return; }
        s = end + 1;
    }
invalid:
    count = 0; fprintf(stderr, "Invalid context registry observation contract; disabled\n");
}

static uint64_t pointer(uint64_t address) {
    uint64_t v = 0;
    return address && plat_mem_read(address, &v, sizeof v) ? v : 0;
}

// Registry map: keys u32*, values u8*, capacity u32 at +0x14. Values select a bit in the manager's +0x30 bitset.
// This mirrors the bounds and open-addressed lookup observed in the fingerprinted gameplay consumers, with extra limits.
static int flag(uint64_t manager, uint32_t hash) {
    uint64_t table = manager ? pointer(manager + 0x40) : 0;
    uint32_t capacity = 0;
    if (!table || !plat_mem_read(table + 0x14, &capacity, 4) || !capacity || capacity > 65536) return -1;
    uint64_t keys = pointer(table), values = pointer(table + 8);
    if (!keys || !values) return -1;
    uint32_t index = hash % capacity;
    for (unsigned step = 0; step < capacity && step < 256; step++) {
        uint32_t key = 0;
        if (!plat_mem_read(keys + (uint64_t)index * 4, &key, 4)) return -1;
        if (key == hash) {
            uint8_t bit = 0; uint32_t word = 0;
            // The observed map pointer begins at +0x40, so no read may extend into it as a bitset word.
            if (!plat_mem_read(values + index, &bit, 1) || bit >= 128 ||
                !plat_mem_read(manager + 0x30 + (bit / 32) * 4, &word, 4)) return -1;
            return (word >> (bit % 32)) & 1;
        }
        if (!key) return -1;
        if (++index == capacity) index = 0;
    }
    return -1;
}

static void update_activation(int status) {
    if (!block_enabled) return;
    uint64_t before = atomic_load(&activation), value = (uint64_t)(status + 1);
    if ((before & 3) != value) {
        uint64_t generation = ((before & ~(1ull << 63)) >> 2) + 1;
        atomic_store(&activation, (1ull << 63) | (generation << 2) | value);
    }
}

// Zero means no explicit gate configured. Otherwise low bits are 0 unknown, 1 clear, 2 paused; upper bits retain
// transitions observed while the gameplay consumer was inactive. Refresh before a consumer can apply its action.
uint64_t input_context_activation(void) {
    pthread_once(&once, init);
    if (!block_enabled) return 0;
    pthread_mutex_lock(&lock);
    update_activation(flag(pointer(global), block_hash));
    uint64_t result = atomic_load(&activation);
    pthread_mutex_unlock(&lock);
    return result;
}

int input_context_flag(uint32_t hash) {
    pthread_once(&once,init);
    if(!global)return -1;
    pthread_mutex_lock(&lock);int result=flag(pointer(global),hash);pthread_mutex_unlock(&lock);
    return result;
}

static void registry_snapshot(uint64_t manager) {
    uint32_t words[4];
    if (manager && plat_mem_read(manager + 0x30, words, sizeof words))
        for (unsigned i = 0; i < 4; i++) input_context_word_observe(global, i, words[i]);
    uint64_t table = manager ? pointer(manager + 0x40) : 0;
    if (!table || (manager == previous_manager && table == previous_table)) return;
    uint32_t capacity = 0;
    if (!plat_mem_read(table + 0x14, &capacity, 4) || !capacity || capacity > 65536) return;
    uint64_t keys = pointer(table), values = pointer(table + 8);
    if (!keys || !values) return;
    unsigned emitted = 0;
    for (uint32_t i = 0; i < capacity && i < 4096 && emitted < 128; i++) {
        uint32_t hash = 0; uint8_t bit = 0;
        if (!plat_mem_read(keys + (uint64_t)i * 4, &hash, 4)) return;
        if (!hash) continue;
        if (!plat_mem_read(values + i, &bit, 1)) return;
        if (bit < 128) { input_context_entry_observe(global, hash, bit); emitted++; }
    }
    previous_manager = manager; previous_table = table;
}

void input_context_tick(void) {
    pthread_once(&once, init);
    int64_t clock = frame_step_world_us();
    if (!count || clock < 0) return;
    pthread_mutex_lock(&lock);
    if (previous >= 0 && clock >= previous && clock - previous < 10000) { pthread_mutex_unlock(&lock); return; }
    previous = clock;
    uint64_t manager = pointer(global);
    registry_snapshot(manager);
    for (unsigned i = 0; i < count; i++) {
        int status = flag(manager, hashes[i]);
        input_context_observe(global, hashes[i], status);
        if (block_enabled && hashes[i] == block_hash) update_activation(status);
    }
    pthread_mutex_unlock(&lock);
}
