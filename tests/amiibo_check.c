// Synthetic tags only. Run with python tests/amiibo_check.py.
#include <assert.h>
#include "../runtime/amiibo.c"
#include "../runtime/nfp.c"

static uint8_t file_data[8192];
static size_t file_size;
static uint64_t now, last_system_event;
static unsigned signals[8];
static uint32_t next_event;

void rt_log(const char *fmt, ...) { (void)fmt; }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
uint64_t plat_now_ns(void) { return now; }
uint8_t *plat_file_read(const char *path, uint64_t *size) {
    (void)path;
    uint8_t *p = malloc(file_size + 1);
    assert(p);
    memcpy(p, file_data, file_size);
    p[file_size] = 0;
    *size = file_size;
    return p;
}
void kern_signal_system_event(uint64_t ev) { last_system_event = ev; }
bool kern_try_signal_system_event(uint64_t ev) { kern_signal_system_event(ev); return true; }
void kern_event_signal(uint32_t ev) { assert(ev < 8); ++signals[ev]; }
uint32_t kern_event_new(void) { return ++next_event; }
uint32_t kern_event_readable(uint32_t ev) { return ev; }
void kern_handle_close(uint32_t ev) { (void)ev; }
uint32_t ipc_session_new(const char *n, IpcHandler h, void *p, void (*close)(void *)) {
    (void)n; (void)h; (void)p; (void)close; return 1;
}
void *ipc_guest(uint64_t addr, uint64_t size) { (void)size; return (void *)(uintptr_t)addr; }
void ipc_out(IpcResponse *rs, const void *p, uint32_t n) { memcpy(rs->data, p, n); rs->data_size = n; }
void ipc_unimplemented(const IpcSession *s, const IpcRequest *rq) { (void)s; (void)rq; abort(); }
void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }

int main(void) {
    uint8_t tag[572] = {4, 1, 2, 0x8f, 3, 4, 5, 6, 4};
    tag[12] = 0xf1;
    const uint8_t id[] = {0x12, 0x34, 0x56, 0, 0x78, 0x9a, 0x0b, 2};
    memcpy(tag + 0x54, id, sizeof id);
    amiibo_init();
    nfp_set_system_event(true, 100);
    nfp_set_system_event(false, 200);
    IpcRequest rq = {0};
    IpcResponse rs = {0};
    nfp_user_manager_handler(NULL, &rq, &rs);
    assert(users && signals[users->avail_event] == 0);
    amiibo_start_detection();
    assert(amiibo_take_picker_request() && !amiibo_take_picker_request());
    amiibo_start_detection();
    assert(!amiibo_take_picker_request());
    amiibo_stop_detection();
    amiibo_start_detection();
    assert(amiibo_take_picker_request());
    const unsigned sizes[] = {532, 540, 572};
    for (unsigned i = 0; i < 3; ++i) {
        memcpy(file_data, tag, sizeof tag);
        file_size = sizes[i];
        assert(amiibo_scan_and_tap("synthetic.bin"));
        assert(last_system_event == 100 && signals[users->activate_event] == i + 1);
        uint8_t info[0x58], model[0x40];
        amiibo_get_tag_info(info);
        const uint8_t uid[] = {4, 1, 2, 3, 4, 5, 6};
        assert(!memcmp(info, uid, sizeof uid) && info[10] == 7);
        assert(amiibo_mount());
        amiibo_get_model_info(model);
        const uint8_t expected[] = {0x34, 0x12, 0x56, 0x0b, 0x9a, 0x78, 0};
        assert(!memcmp(model, expected, sizeof expected));
        // Exercise the receive-pointer IPC path used by the SDK, with an overrun sentinel.
        uint8_t record[0x101];
        const unsigned cmds[] = {13, 14, 15, 16};
        const unsigned lengths[] = {0x58, 0x100, 0x40, 0x40};
        IpcSession session = {.state = users};
        for (unsigned j = 0; j < 4; ++j) {
            memset(record, 0xaa, sizeof record);
            rq = (IpcRequest){.cmd = cmds[j], .nc = 1};
            rq.c[0] = (IpcBuffer){(uintptr_t)record, lengths[j], 0};
            rs = (IpcResponse){0};
            nfp_user_handler(&session, &rq, &rs);
            assert(!rs.result && rs.nxout == 1 && record[lengths[j]] == 0xaa);
            if (j == 0) assert(!memcmp(record, uid, sizeof uid));
            if (j == 3) assert(!memcmp(record, expected, sizeof expected));
            rq.c[0].size = 1;
            rs = (IpcResponse){0};
            nfp_user_handler(&session, &rq, &rs);
            assert(rs.result != 0 && !rs.nxout);
        }
        amiibo_remove_tag();
        assert(last_system_event == 200 && signals[users->deactivate_event] == i + 1);
        assert(amiibo_get_device_state() == AMIIBO_DEV_STATE_TAG_REMOVED);
    }
    // Flipper exports, including truncated exports and bad check bytes.
    file_size = (size_t)sprintf((char *)file_data, "Filetype: Flipper NFC device\n");
    for (unsigned page = 0; page < 133; ++page)
        file_size += (size_t)sprintf((char *)file_data + file_size, "Page %u: %02X %02X %02X %02X\n",
                                    page, tag[page*4], tag[page*4+1], tag[page*4+2], tag[page*4+3]);
    assert(amiibo_scan_and_tap("synthetic.nfc"));
    now = 3000000000ull;
    amiibo_tick();
    assert(!amiibo_has_tag() && last_system_event == 200);
    file_size -= 25;
    assert(!amiibo_load_file("truncated.nfc"));
    memcpy(file_data, tag, sizeof tag);
    file_size = 540;
    file_data[3] ^= 1;
    assert(!amiibo_load_file("invalid.bin"));
    file_size = 1;
    assert(!amiibo_load_file("short.bin"));
    IpcSession session = {.state = users};
    rq = (IpcRequest){.cmd = 999};
    nfp_user_handler(&session, &rq, &rs);
    assert(rs.result == IPC_R_UNKNOWN_COMMAND);
    amiibo_stop_detection();
    assert(!amiibo_take_picker_request() && amiibo_get_device_state() == AMIIBO_DEV_STATE_INITIALIZED);
    amiibo_start_detection();
    assert(amiibo_take_picker_request());
    // A tap selected while detection is between retries survives until the game mounts it.
    memcpy(file_data, tag, sizeof tag);
    file_size = sizeof tag;
    amiibo_stop_detection();
    assert(amiibo_scan_and_tap("pending.bin"));
    amiibo_start_detection();
    assert(amiibo_has_tag() && !amiibo_take_picker_request());
    amiibo_stop_detection();
    amiibo_start_detection();
    assert(amiibo_has_tag() && amiibo_mount());
    amiibo_stop_detection();
    assert(!amiibo_has_tag());
    nfp_user_close(users);
    assert(!users);
    // E1 and F1 headers must produce the same identity without rewriting the input.
    uint8_t first_uid[7], first_model[0x40];
    for (unsigned i = 0; i < 2; ++i) {
        memcpy(file_data, tag, sizeof tag);
        file_size = 540;
        file_data[12] = i ? 0xe1 : 0xf1;
        assert(amiibo_load_file("synthetic.bin"));
        assert(amiibo_get_raw_data()[12] == file_data[12]);
        uint8_t model[0x40];
        amiibo_get_model_info(model);
        if (i == 0) {
            memcpy(first_uid, amiibo_get_current_info()->uuid, sizeof first_uid);
            memcpy(first_model, model, sizeof first_model);
        } else {
            assert(!memcmp(first_uid, amiibo_get_current_info()->uuid, sizeof first_uid));
            assert(!memcmp(first_model, model, sizeof first_model));
        }
    }
    puts("amiibo: formats, UID/model ABI, repeated scans, lifecycle, events and IPC failures passed");
    return 0;
}
