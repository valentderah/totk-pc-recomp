// Adapted from MacNativeGames games/totk/runtime/services.c (lines 208-302), applet_host.c, account.c, time_host.c and
// shared_font.c @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames contributors).
// System service overrides: nn::oe, applet, am, prepo, nfp, err, aoc, pctl, diag,
// account, time and pl, and the six observers. Nothing here talks to a Horizon service; each call is answered by the
// host. Stub rule: queries and initialisation succeed with neutral values, data paths stop with a named fatal.
//
// Environment:
//   TOTK_LANG=<Switch language code>   the game's language as given ("ja", "en-GB", "zh-Hant", ...)
//   TOTK_LOCALE=<BCP 47 tag>           else the language for this locale ("pt-BR"), else the Windows user locale;
//                                      a language the game does not ship becomes en-US
//   TOTK_OPERATION_MODE=docked|handheld   operation and performance mode reported to the game (default docked)
//   TOTK_ASSUME_FOCUS=1|0              1 (default, the Game tab's row): GetCurrentFocusState always reports in focus,
//                                      whatever the window says; 0: out of focus while the window is, as on a Switch
//   TOTK_SHARED_FONTS=<dir>            nn::pl shared fonts (tools/extract_fonts.py output), default
//                                      <romfs>/../../firmware/shared-fonts
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfg.h"
#include "fs.h"
#include "host.h"
#include "ov.h"
#include "platform/platform.h"

#define NFP_R_DEVICE_NOT_FOUND 0x8073u     // nfp: no NFC reader on the host
#define ACCOUNT_R_INVALID 0x7c67u          // MacNativeGames' result for an unknown user handle or uid
#define TIME_R_INVALID 0x74u
#define TIME_R_SOURCE_MISMATCH 0x18074u

static void need(int ok, const char *what) {
    if (!ok) rt_fatal("%s", what);
}

static const char *caller(Cpu *c, char *buf, int n) { return ldr_describe(c->x[30] - 4, buf, n); }

// ==== nn::oe (services.c)
// TOTK_OPERATION_MODE, read once: 1 handheld (OperationMode 0, PerformanceMode Normal 0), 0 docked (1, Boost 1).
int ov_handheld(void) {
    static volatile int32_t v = -1;
    int32_t h = v;
    if (h < 0) {
        const char *e = cfg_value("TOTK_OPERATION_MODE");
        if (!strcmp(e, "docked")) h = 0;
        else if (!strcmp(e, "handheld")) h = 1;
        else rt_fatal("TOTK_OPERATION_MODE must be docked or handheld, not '%s'", e);
        if (plat_cas32(&v, h, -1) == -1)
            rt_log("oe: operation mode reported to the game: %s", h ? "handheld (performance normal)"
                                                                  : "docked (performance boost)");
    }
    return (int)h;
}

// The Switch language for a BCP 47 tag; en-US for a language the game does not ship.
static const char *switch_language(const char *tag) {
    char lang[8] = "", region[4] = "", script[8] = "";
    size_t n = strcspn(tag, "-_");
    if (n < sizeof lang)
        for (size_t i = 0; i < n; i++) lang[i] = (char)tolower((unsigned char)tag[i]);
    for (const char *p = tag + n; *p;) {
        p += strspn(p, "-_");
        size_t k = strcspn(p, "-_");
        if (k == 2 && isalpha((unsigned char)p[0]) && isalpha((unsigned char)p[1])) {
            region[0] = (char)toupper((unsigned char)p[0]);
            region[1] = (char)toupper((unsigned char)p[1]);
        } else if (k == 3 && isdigit((unsigned char)p[0])) {
            memcpy(region, p, 3);   // UN M.49 area, e.g. es-419
        } else if (k == 4 && !*script) {
            for (size_t i = 0; i < 4; i++) script[i] = (char)(i ? tolower((unsigned char)p[i]) : toupper((unsigned char)p[i]));
        }
        p += k;
    }
    if (!strcmp(lang, "ja") || !strcmp(lang, "de") || !strcmp(lang, "it") || !strcmp(lang, "nl") ||
        !strcmp(lang, "ru") || !strcmp(lang, "ko") || !strcmp(lang, "pt"))
        return !strcmp(lang, "ja") ? "ja" : !strcmp(lang, "de") ? "de" : !strcmp(lang, "it") ? "it"
             : !strcmp(lang, "nl") ? "nl" : !strcmp(lang, "ru") ? "ru" : !strcmp(lang, "ko") ? "ko" : "pt";
    if (!strcmp(lang, "fr")) return !strcmp(region, "CA") ? "fr-CA" : "fr";
    if (!strcmp(lang, "es")) return *region && strcmp(region, "ES") ? "es-419" : "es";
    if (!strcmp(lang, "zh"))
        return !strcmp(script, "Hant") || !strcmp(region, "TW") || !strcmp(region, "HK") || !strcmp(region, "MO")
                   ? "zh-Hant" : "zh-Hans";
    if (!strcmp(lang, "en"))
        return !strcmp(region, "GB") || !strcmp(region, "IE") || !strcmp(region, "AU") || !strcmp(region, "NZ")
                   ? "en-GB" : "en-US";
    return "en-US";
}

// The game's language as a Switch language code (TOTK_LANG, else TOTK_LOCALE, else the Windows user locale). TotK
// accepts "zh-Hans" and "zh-Hant" for Chinese and falls back to Japanese for "zh-CN" and "zh-TW", so those two
// spellings, which older totk.ini files and menus wrote, are mapped to them.
const char *ov_language(void) {
    static const char *const codes[] = {"ja", "en-US", "fr", "de", "it", "es", "ko", "nl", "pt", "ru",
                                        "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
    const char *code = cfg_get("TOTK_LANG");   // no fixed default: TOTK_LOCALE or the Windows user locale decides when unset
    if (code && *code) {
        if (!strcmp(code, "zh-CN")) return "zh-Hans";
        if (!strcmp(code, "zh-TW")) return "zh-Hant";
        for (size_t i = 0; i < sizeof codes / sizeof *codes; i++)
            if (!strcmp(code, codes[i])) return codes[i];
        rt_fatal("TOTK_LANG '%s' is not a Switch language code", code);
    }
    const char *tag = cfg_get("TOTK_LOCALE");
    char sys[85] = "en-US";
    if (!tag || !*tag) {
        if (*plat_user_locale()) snprintf(sys, sizeof sys, "%s", plat_user_locale());
        tag = sys;
    }
    return switch_language(tag);
}

// LanguageCode GetDesiredLanguage(): char[8] in x0
static void ov_desired_language(Cpu *c) {
    const char *code = ov_language();
    uint64_t v = 0;
    memcpy(&v, code, strlen(code));
    ret_x(c, v);
}

static void ov_oe_initialize(Cpu *c) { (void)c; rt_log("oe: application environment: a local host process"); }

// void SetPerformanceConfiguration(PerformanceMode mode, int config)
static void ov_performance_config(Cpu *c) {
    int64_t mode = (int32_t)arg_x(c, 0);
    if (mode < 0 || mode > 1) rt_fatal("nn::oe::SetPerformanceConfiguration: invalid mode %lld", (long long)mode);
    rt_log("oe: performance request mode %lld configuration %#x: the host OS schedules the CPU", (long long)mode,
           (uint32_t)arg_x(c, 1));
}

// void SetCpuBoostMode(CpuBoostMode mode)
static void ov_cpu_boost(Cpu *c) {
    int64_t mode = (int32_t)arg_x(c, 0);
    if (mode < 0 || mode > 2) rt_fatal("nn::oe::SetCpuBoostMode: unknown mode %lld", (long long)mode);
}

static void ov_operation_mode(Cpu *c) { ret_x(c, ov_handheld() ? 0 : 1); }
static void ov_performance_mode(Cpu *c) { ret_x(c, ov_handheld() ? 0 : 1); }

// void SetFocusHandlingMode(FocusHandlingMode mode): the nnMain checkpoint of --boot
static volatile int32_t focus_mode;
static void ov_focus_mode(Cpu *c) {
    int64_t mode = (int32_t)arg_x(c, 0);
    if (mode < 0 || mode > 3) rt_fatal("nn::oe::SetFocusHandlingMode: unknown mode %lld", (long long)mode);
    plat_xchg32(&focus_mode, (int32_t)mode);
    if (rt_config()->boot) {
        rt_boot_event("checkpoint", c, "SetFocusHandlingMode");
    } else {
        char lr[96];
        rt_log("oe: focus handling mode %lld from %s", (long long)mode, caller(c, lr, sizeof lr));
    }
}

// FocusState GetCurrentFocusState(): 1 in focus, 2 out of focus (nn::oe::FocusState). The game sets focus handling
// mode 2: on a Switch the system then suspends the whole process while it is out of focus and sends Resume when it
// returns. Here only the game's main thread stops (it waits in PopNotificationMessage for the next message) while its
// loader threads, audio and clocks run on, and a load interrupted that way was seen to never finish (Linux, 1.4.3).
// So TOTK_ASSUME_FOCUS=1, the default, keeps it at 1, as MacNativeGames does; with 0 it follows the window through
// oe_set_focus. The caller of oe_set_focus also pushes OE_MSG_FOCUS_STATE_CHANGED.
//
// The setting is read on every focus change rather than once at start: a focus change is rare, the lookup is a short
// walk of the loaded ini entries, and the Game tab's row then takes effect the next time the window is left, without
// a restart.
static volatile int32_t focus_state = 1;
void oe_set_focus(int in_focus) {
    plat_xchg32(&focus_state, in_focus || atoi(cfg_value("TOTK_ASSUME_FOCUS")) ? 1 : 2);
}
static void ov_focus_state(Cpu *c) { ret_x(c, (uint64_t)focus_state); }

static volatile int32_t notify_resume, notify_operation, notify_performance;
static void ov_resume_enabled(Cpu *c) { plat_xchg32(&notify_resume, (int32_t)(arg_x(c, 0) & 0xff)); }
static void ov_operation_enabled(Cpu *c) { plat_xchg32(&notify_operation, (int32_t)(arg_x(c, 0) & 0xff)); }
static void ov_performance_enabled(Cpu *c) { plat_xchg32(&notify_performance, (int32_t)(arg_x(c, 0) & 0xff)); }

// ==== nn::oe, applet, am, prepo, nfp, err (applet_host.c)
static PlatMutex lock = PLAT_MUTEX_INIT;
static PlatCond message_cv = PLAT_COND_INIT;
static uint32_t messages[32];
static unsigned head, tail;
static float balance[2] = {1.0f, 1.0f};
static uint64_t start_ticks;
__attribute__((constructor)) static void started(void) { start_ticks = rt_ticks(); }

void oe_push_message(uint32_t message) {
    plat_mutex_lock(&lock);
    if (tail - head < 32) {
        messages[tail++ % 32] = message;
        plat_cond_broadcast(&message_cv);
    }
    plat_mutex_unlock(&lock);
}

// void GetExpectedVolumeBalance(float *left, float *right)
static void ov_get_balance(Cpu *c) {
    float *l = arg_p(c, 0), *r = arg_p(c, 1);
    plat_mutex_lock_shared(&lock);
    if (l) *l = balance[0];
    if (r) *r = balance[1];
    plat_mutex_unlock_shared(&lock);
}

// void SetExpectedVolumeBalance(float left, float right)
static void ov_set_balance(Cpu *c) {
    float l = arg_s(c, 0), r = arg_s(c, 1);
    if (!(l >= 0 && l <= 1 && r >= 0 && r <= 1))
        rt_fatal("nn::oe::SetExpectedVolumeBalance: balance %g/%g out of range", (double)l, (double)r);
    plat_mutex_lock(&lock);
    balance[0] = l;
    balance[1] = r;
    plat_mutex_unlock(&lock);
}

// TimeSpan GetProgramTotalActiveTime(): nanoseconds since the process started (19.2 MHz ticks * 625 / 12)
static void ov_active_time(Cpu *c) {
    ret_x(c, (uint64_t)((unsigned __int128)(rt_ticks() - start_ticks) * 625 / 12));
}

// void SetUserInactivityDetectionTimeExtended[Unsafe](bool): the host never puts the console to sleep
static void ov_inactivity_extended(Cpu *c) { (void)c; }

// void FinishStartupLogo(): no system logo is drawn on the host
static void ov_finish_logo(Cpu *c) { (void)c; rt_log("oe: the game finished its startup logo"); }

// bool TryPopLaunchParameter(size_t *size, void *buffer, size_t capacity): launched directly, no parameter
static void ov_launch_parameter(Cpu *c) {
    uint64_t *size = arg_p(c, 0);
    if (size) *size = 0;
    ret_x(c, 0);
}

// bool TryPopNotificationMessage(Message *out)
static void ov_try_pop(Cpu *c) {
    uint32_t *out = arg_p(c, 0);
    plat_mutex_lock(&lock);
    int ok = head != tail;
    if (ok) {
        uint32_t m = messages[head++ % 32];
        if (out) *out = m;
    }
    plat_mutex_unlock(&lock);
    ret_x(c, (uint64_t)ok);
}

// Message PopNotificationMessage(): blocks like a Horizon wait, so the thread gives its virtual core up meanwhile
static void ov_pop(Cpu *c) {
    int away = kern_core_away();
    plat_mutex_lock(&lock);
    while (head == tail) plat_cond_wait(&message_cv, &lock);
    uint32_t m = messages[head++ % 32];
    plat_mutex_unlock(&lock);
    kern_core_back(&away);
    ret_x(c, m);
}

// int applet::GetPreviousProgramIndex(): started directly, not from an ExecuteProgram chain
static void ov_previous_program(Cpu *c) { ret_x(c, (uint64_t)(int64_t)-1); }
// AppletResourceUserId applet:: / am::GetAppletResourceUserId(): one application, one stable non-zero id
static void ov_aruid(Cpu *c) { ret_x(c, 1); }

// nn::prepo: play reports stay local (no Nintendo servers)
static volatile int64_t reports;
static void ov_report_save(Cpu *c) {   // Result PlayReport::Save() and Save(const account::Uid &)
    int64_t n = plat_inc64(&reports);
    if (n % 100 == 1) rt_log("prepo: play report kept local (not sent), %lld so far", (long long)n);
    ret_x(c, 0);
}
static void ov_result_ok(Cpu *c) { ret_x(c, 0); }   // RequestImmediateTransmission, AddInternetConnectionStatus

// nn::nfp: no NFC reader
static volatile int32_t nfp_state;
static void ov_nfp_initialize(Cpu *c) { plat_xchg32(&nfp_state, 1); ret_x(c, 0); }
static void ov_nfp_finalize(Cpu *c) { plat_xchg32(&nfp_state, 0); ret_x(c, 0); }
static void ov_nfp_state(Cpu *c) { ret_x(c, (uint64_t)nfp_state); }
// Result ListDevices(DeviceHandle *out, int *count, int max)
static void ov_nfp_list(Cpu *c) {
    int32_t *count = arg_p(c, 1);
    if (count) *count = 0;
    ret_x(c, NFP_R_DEVICE_NOT_FOUND);
}
// Result AttachAvailabilityChangeEvent(os::SystemEventType *event)
static void ov_nfp_availability(Cpu *c) {
    uint64_t ev = arg_x(c, 0);
    if (!ev) rt_fatal("nn::nfp::AttachAvailabilityChangeEvent: null event");
    kern_bind_system_event(c, ev, 0);
    ret_x(c, 0);
}
static void ov_nfp_no_device(Cpu *c) { ret_x(c, NFP_R_DEVICE_NOT_FOUND); }
static void ov_nfp_device_state(Cpu *c) { ret_x(c, 0); }   // DeviceState GetDeviceState(const DeviceHandle &): none

// void err::ShowError(Result): the game's error dialog, logged
static void ov_show_error(Cpu *c) {
    uint32_t r = (uint32_t)arg_x(c, 0);
    char lr[96];
    rt_log("err: game error dialog %04u-%04u (result %#x) from %s", 2000 + (r & 0x1ff), (r >> 9) & 0x1fff, r,
           caller(c, lr, sizeof lr));
}

// ==== SDK-internal initialisation (services.c)
static void ov_diag_startup(Cpu *c) { (void)c; rt_log("diag: host diagnostics"); }
static void ov_aoc_initialize(Cpu *c) { (void)c; rt_log("aoc: no add-on content"); }
// 1.4.3's main lists add-on content (MNG services.c host_aoc_list): there is none.
// int ListAddOnContent(AddOnContentIndex *out, int offset, int count)
static void ov_aoc_list(Cpu *c) {
    int offset = (int)arg_x(c, 1), count = (int)arg_x(c, 2);
    if (offset < 0 || count < 0) rt_fatal("aoc: ListAddOnContent range %d+%d", offset, count);
    ret_x(c, 0);
}
static void ov_err_context(Cpu *c) { (void)c; rt_log("err: error contexts stay local"); }
static void ov_pctl_initialize(Cpu *c) { (void)c; rt_log("pctl: no parental controls"); }
static void ov_abort_observer(Cpu *c) { (void)c; rt_log("diag: the host crash reporter observes aborts"); }

// ==== observers (services.c): log, then the guest definition
static void observe(Cpu *c, const char *name) {
    rt_log("ENTER %s", name);
    rt_original(c);
    rt_log("RETURN %s", name);
}
static void obs_os_initialize(Cpu *c) { observe(c, "nnosInitialize"); }
static void obs_sdk_initialize(Cpu *c) { observe(c, "nninitInitializeSdkModule"); }
static void obs_startup(Cpu *c) { observe(c, "nninitStartup"); }
static void obs_main(Cpu *c) { observe(c, "nnMain"); }

// Result os::AllocateAddressRegion(uintptr_t *out, size_t size)
static void obs_address_region(Cpu *c) {
    uint64_t *out = arg_p(c, 0), size = arg_x(c, 1);
    uint32_t r = (uint32_t)rt_original(c);
    if (rt_trace & RT_TRACE_HOSTCALL)   // 20,000 lines a session otherwise, each a write on a game thread
        rt_log("AllocateAddressRegion size %#llx result %#x address %#llx", (unsigned long long)size, r,
           (unsigned long long)(r || !out ? 0 : *out));
}

// Result os::AllocateMemoryPages(uintptr_t address, size_t size)
static void obs_memory_pages(Cpu *c) {
    uint64_t a = arg_x(c, 0), size = arg_x(c, 1);
    uint32_t r = (uint32_t)rt_original(c);
    if (rt_trace & RT_TRACE_HOSTCALL)
        rt_log("AllocateMemoryPages address %#llx size %#llx result %#x", (unsigned long long)a, (unsigned long long)size, r);
}

// ==== nn::account (account.c): one offline local user whose Uid survives restarts in <profile>/account.uid
typedef struct { uint64_t lo, hi; } Uid;
typedef struct { Uid uid; uint64_t token; } UserHandle;
static PlatMutex account_lock = PLAT_MUTEX_INIT;
static Uid local;
static uint64_t tokens[64], next_token = 1;

static void make_dirs(const char *dir) {
    plat_file_mkdir_p(dir);
    if (!plat_dir_exists(dir)) rt_fatal("account: cannot create %s", dir);
}

// Holding account_lock.
static void load_uid(void) {
    if (local.lo || local.hi) return;
    const char *dir = rt_config()->profile;
    char path[1100];
    make_dirs(dir);
    need(snprintf(path, sizeof path, "%s/account.uid", dir) < (int)sizeof path, "account: path too long");
    int64_t size = plat_file_size(path);
    if (size <= 0) {
        do need(plat_random(&local, sizeof local), "account: random uid");
        while (!local.lo && !local.hi);
        need(plat_file_write_atomic(path, &local, sizeof local), "account: persist the local account identity");
        rt_log("account: new local account in %s", path);
    } else {
        uint64_t got = 0;
        uint8_t *b = plat_file_read(path, &got);
        if (b && got == sizeof local) memcpy(&local, b, sizeof local);
        free(b);
        if (got != sizeof local || (!local.lo && !local.hi))
            rt_fatal("account: %s is not a local account identity (16 bytes)", path);
    }
}

// host.h: the save directory of fs_host.c is named after this Uid.
void account_uid(uint64_t out[2]) {
    plat_mutex_lock(&account_lock);
    load_uid();
    out[0] = local.lo;
    out[1] = local.hi;
    plat_mutex_unlock(&account_lock);
}

static int same_uid(const Uid *u) { return u && u->lo == local.lo && u->hi == local.hi; }

// Holding account_lock.
static int valid_handle(const UserHandle *h) {
    if (!h || !same_uid(&h->uid)) return 0;
    for (int i = 0; i < 64; i++)
        if (tokens[i] && tokens[i] == h->token) return 1;
    return 0;
}

// void Initialize()
static void ov_account_initialize(Cpu *c) {
    uint64_t uid[2];
    (void)c;
    account_uid(uid);
}

// bool TryOpenPreselectedUser(UserHandle *out)
static void ov_open_user(Cpu *c) {
    UserHandle *out = arg_p(c, 0);
    if (!out) rt_fatal("nn::account::TryOpenPreselectedUser: null handle");
    plat_mutex_lock(&account_lock);
    load_uid();
    int ok = 0;
    for (int i = 0; i < 64 && !ok; i++)
        if (!tokens[i]) {
            tokens[i] = next_token++;
            *out = (UserHandle){local, tokens[i]};
            ok = 1;
        }
    plat_mutex_unlock(&account_lock);
    ret_x(c, (uint64_t)ok);
}

// void CloseUser(const UserHandle &)
static void ov_close_user(Cpu *c) {
    const UserHandle *h = arg_p(c, 0);
    plat_mutex_lock(&account_lock);
    if (!valid_handle(h)) rt_fatal("nn::account::CloseUser: not an open user handle");
    for (int i = 0; i < 64; i++)
        if (tokens[i] == h->token) tokens[i] = 0;
    plat_mutex_unlock(&account_lock);
}

// Result GetUserId(Uid *out, const UserHandle &)
static void ov_user_id(Cpu *c) {
    Uid *out = arg_p(c, 0);
    plat_mutex_lock_shared(&account_lock);
    int ok = out && valid_handle(arg_p(c, 1));
    if (ok) *out = local;
    plat_mutex_unlock_shared(&account_lock);
    ret_x(c, ok ? 0 : ACCOUNT_R_INVALID);
}

// Result GetUserExistence(bool *out, const Uid &)
static void ov_user_existence(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    const Uid *u = arg_p(c, 1);
    if (!out || !u) { ret_x(c, ACCOUNT_R_INVALID); return; }
    plat_mutex_lock(&account_lock);
    load_uid();
    *out = (uint8_t)same_uid(u);
    plat_mutex_unlock(&account_lock);
    ret_x(c, 0);
}

// Result GetNickname(Nickname *out, const Uid &): 33 bytes, NUL-terminated UTF-8
static void ov_nickname(Cpu *c) {
    char *out = arg_p(c, 0);
    plat_mutex_lock(&account_lock);
    load_uid();
    int ok = out && same_uid(arg_p(c, 1));
    plat_mutex_unlock(&account_lock);
    if (ok) {
        memset(out, 0, 33);
        memcpy(out, "Local", 5);
    }
    ret_x(c, ok ? 0 : ACCOUNT_R_INVALID);
}

// ==== nn::time (time_host.c): the host clock and time zone. User and network clocks follow the host wall clock; the
// steady clock counts seconds since Windows started, with one random source id per process. ClockSnapshot has the
// public 0xD0-byte layout, because game code reads its fields through inline accessors.
typedef struct { int64_t value; uint8_t source[16]; } SteadyClockTimePoint;
typedef struct { int64_t offset; SteadyClockTimePoint point; } SystemClockContext;
typedef struct { int16_t year; int8_t month, day, hour, minute, second, pad; } CalendarTime;
typedef struct { int32_t day_of_week, day_of_year; char name[8]; uint8_t dst, pad[3]; int32_t utc_offset; } CalendarInfo;
typedef struct {
    SystemClockContext user, network;
    int64_t user_time, network_time;
    CalendarTime user_calendar, network_calendar;
    CalendarInfo user_info, network_info;
    SteadyClockTimePoint steady;
    char location[0x24];
    uint8_t automatic_correction, type, pad[2];
} ClockSnapshot;
_Static_assert(sizeof(SteadyClockTimePoint) == 0x18 && sizeof(CalendarTime) == 8 && sizeof(CalendarInfo) == 0x18,
               "nn::time types");
_Static_assert(sizeof(ClockSnapshot) == 0xd0, "nn::time::ClockSnapshot");

static uint8_t source_id[16];
static volatile int32_t time_ready;
static PlatOnce source_once = PLAT_ONCE_INIT;

static void make_source(void *param) {
    (void)param;
    need(plat_random(source_id, sizeof source_id), "time: source id");
    plat_tz_west();   // reads the zone once, before the first localtime
    return;
}

static SteadyClockTimePoint steady_now(void) {
    plat_once(&source_once, make_source, NULL);
    SteadyClockTimePoint p = {(int64_t)(plat_now_ms() / 1000), {0}};
    memcpy(p.source, source_id, 16);
    return p;
}

static int64_t posix_now(void) { return (int64_t)time(NULL); }

// Days since 1970-01-01 of a proleptic Gregorian date, and back (H. Hinnant's civil calendar algorithms).
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int64_t)yoe + era * 400 + (*m <= 2);
}

static int64_t floor_div(int64_t a, int64_t b) { return a / b - (a % b != 0 && (a < 0) != (b < 0)); }

// UTC calendar fields of a POSIX time. 0 if the year does not fit CalendarTime.
static int utc_calendar(int64_t t, CalendarTime *c, CalendarInfo *info) {
    int64_t days = floor_div(t, 86400), sec = t - days * 86400, y;
    unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    *c = (CalendarTime){(int16_t)y, (int8_t)m, (int8_t)d, (int8_t)(sec / 3600), (int8_t)(sec / 60 % 60),
                        (int8_t)(sec % 60), 0};
    if (info) {
        memset(info, 0, sizeof *info);
        info->day_of_week = (int32_t)(days + 4 - floor_div(days + 4, 7) * 7);   // 1970-01-01 was a Thursday
        info->day_of_year = (int32_t)(days - days_from_civil(y, 1, 1));
        memcpy(info->name, "UTC", 3);
    }
    return y >= -32768 && y <= 32767;
}

// Local calendar fields of a POSIX time in the Windows time zone. The zone name is "UTC" or the UTC offset ("+03",
// "-0330"), as tzdata names zones without an abbreviation; Windows has no abbreviations. 0 outside localtime's range.
static int local_calendar(int64_t t, CalendarTime *c, CalendarInfo *info) {
    plat_once(&source_once, make_source, NULL);
    struct tm tm;
    if (!plat_localtime(t, &tm)) return 0;
    int64_t as_utc = days_from_civil(tm.tm_year + 1900, (unsigned)tm.tm_mon + 1, (unsigned)tm.tm_mday) * 86400 +
                     tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    int32_t offset = (int32_t)(as_utc - t);
    *c = (CalendarTime){(int16_t)(tm.tm_year + 1900), (int8_t)(tm.tm_mon + 1), (int8_t)tm.tm_mday, (int8_t)tm.tm_hour,
                        (int8_t)tm.tm_min, (int8_t)tm.tm_sec, 0};
    if (info) {
        memset(info, 0, sizeof *info);
        info->day_of_week = tm.tm_wday;
        info->day_of_year = tm.tm_yday;
        int a = offset < 0 ? -offset : offset;
        if (!offset) memcpy(info->name, "UTC", 3);
        else if (a % 3600) snprintf(info->name, sizeof info->name, "%c%02d%02d", offset < 0 ? '-' : '+', a / 3600, a / 60 % 60);
        else snprintf(info->name, sizeof info->name, "%c%02d", offset < 0 ? '-' : '+', a / 3600);
        info->dst = tm.tm_isdst > 0;
        info->utc_offset = offset;
    }
    return 1;
}

// IANA name of the standard offset: Etc/GMT+5 is UTC-5 (POSIX sign); "UTC" for a zone off whole hours.
static void location_name(char out[0x24]) {
    long west = plat_tz_west();
    memset(out, 0, 0x24);
    if (!west || west % 3600) snprintf(out, 0x24, "UTC");
    else snprintf(out, 0x24, "Etc/GMT%+ld", west / 3600);
}

static void ov_time_initialize(Cpu *c) {
    plat_once(&source_once, make_source, NULL);
    plat_xchg32(&time_ready, 1);
    ret_x(c, 0);
}
static void ov_time_finalize(Cpu *c) { plat_xchg32(&time_ready, 0); ret_x(c, 0); }
static void ov_time_ready(Cpu *c) { ret_x(c, (uint64_t)time_ready); }

// Result StandardUserSystemClock / StandardNetworkSystemClock::GetCurrentTime(PosixTime *out)
static void ov_current_time(Cpu *c) {
    int64_t *out = arg_p(c, 0);
    if (!out) { ret_x(c, TIME_R_INVALID); return; }
    *out = posix_now();
    ret_x(c, 0);
}

// time_point StandardSteadyClock::now(): the int64 seconds in x0 only. The SDK caller (sdk+0x3d1d88) reads x0;
// returning the 0x18-byte struct through x8 would write through a stale pointer (time_host.c).
static void ov_steady_now(Cpu *c) { ret_x(c, (uint64_t)steady_now().value); }

// Result ToCalendarTime(CalendarTime *out, CalendarAdditionalInfo *info, const PosixTime &t): local time
static void ov_to_calendar(Cpu *c) {
    CalendarTime *out = arg_p(c, 0);
    CalendarInfo *info = arg_p(c, 1);
    const int64_t *t = arg_p(c, 2);
    if (!out || !t) { ret_x(c, TIME_R_INVALID); return; }
    CalendarTime ct;
    CalendarInfo ci;
    if (!local_calendar(*t, &ct, &ci)) { ret_x(c, TIME_R_INVALID); return; }
    *out = ct;
    if (info) *info = ci;
    ret_x(c, 0);
}

// CalendarTime ToCalendarTimeInUtc(const PosixTime &t): 8 bytes in x0
static void ov_to_calendar_utc(Cpu *c) {
    const int64_t *t = arg_p(c, 0);
    CalendarTime ct;
    utc_calendar(t ? *t : 0, &ct, NULL);
    uint64_t v;
    memcpy(&v, &ct, 8);
    ret_x(c, v);
}

// PosixTime ToPosixTimeFromUtc(const CalendarTime &c): months out of 1..12 carry into the year, like timegm
static void ov_to_posix_utc(Cpu *c) {
    const CalendarTime *ct = arg_p(c, 0);
    if (!ct) rt_fatal("nn::time::ToPosixTimeFromUtc: null calendar time");
    int64_t mon = ct->month - 1, y = ct->year + floor_div(mon, 12);
    mon -= floor_div(mon, 12) * 12;
    int64_t t = (days_from_civil(y, (unsigned)mon + 1, 1) + ct->day - 1) * 86400 + ct->hour * 3600 + ct->minute * 60 +
                ct->second;
    ret_x(c, (uint64_t)t);
}

// ClockSnapshot::ClockSnapshot()
static void ov_snapshot_init(Cpu *c) {
    ClockSnapshot *s = arg_p(c, 0);
    if (s) memset(s, 0, sizeof *s);
}

// static Result ClockSnapshot::CreateWithStandardSystemClock(ClockSnapshot *out)
static void ov_snapshot_create(Cpu *c) {
    ClockSnapshot *s = arg_p(c, 0);
    if (!s) { ret_x(c, TIME_R_INVALID); return; }
    SteadyClockTimePoint steady = steady_now();
    int64_t now = posix_now();
    ClockSnapshot n = {0};
    n.user = (SystemClockContext){now - steady.value, steady};
    n.network = n.user;
    n.user_time = n.network_time = now;
    if (!local_calendar(now, &n.user_calendar, &n.user_info)) rt_fatal("nn::time: the host clock is out of range");
    n.network_calendar = n.user_calendar;
    n.network_info = n.user_info;
    n.steady = steady;
    location_name(n.location);
    n.automatic_correction = 1;
    *s = n;
    ret_x(c, 0);
}

// Result CalculateSpanBetween(TimeSpan *out, const ClockSnapshot &a, const ClockSnapshot &b): nanoseconds
static void ov_span_between(Cpu *c) {
    int64_t *out = arg_p(c, 0);
    const ClockSnapshot *a = arg_p(c, 1), *b = arg_p(c, 2);
    if (!out || !a || !b) { ret_x(c, TIME_R_INVALID); return; }
    if (memcmp(a->steady.source, b->steady.source, 16)) { ret_x(c, TIME_R_SOURCE_MISMATCH); return; }
    *out = (b->steady.value - a->steady.value) * 1000000000ll;
    ret_x(c, 0);
}

// TimeSpan CalculateStandardUserSystemClockDifferenceByUser(const ClockSnapshot &a, const ClockSnapshot &b): the
// change of the user clock offset between the snapshots (manual adjustments) in nanoseconds
static void ov_user_difference(Cpu *c) {
    const ClockSnapshot *a = arg_p(c, 0), *b = arg_p(c, 1);
    if (!a || !b) rt_fatal("nn::time::CalculateStandardUserSystemClockDifferenceByUser: null snapshot");
    if (memcmp(a->user.point.source, b->user.point.source, 16)) { ret_x(c, 0); return; }
    ret_x(c, (uint64_t)((b->user.offset - a->user.offset) * 1000000000ll));
}

// ==== nn::pl shared fonts (shared_font.c). TotK asks for them only from its font loader, only with Simplified Chinese,
// Korean or Traditional Chinese text, and only for type 0; tools/extract_fonts.py writes
// them from the user's firmware. Each type is read on first use into guest-visible memory as the BFTTF file the pl
// shared memory holds; Address is 8 bytes past the file start (after the BFTTF header) and Size the decoded font size,
// as in MNG. The load state is always Loaded: the game spins until it is, so missing files stop with a named fatal at
// Address/Size instead of a hang. TOTK_SHARED_FONTS=<dir> overrides <romfs>/../../firmware/shared-fonts.
#define SHARED_FONT_TYPES 6
#define BFTTF_MAGIC 0x7f9a0218u
static const char *const font_files[SHARED_FONT_TYPES] = {
    "0-standard.bfttf", "1-chinese-simplified.bfttf", "2-chinese-simplified-extension.bfttf",
    "3-chinese-traditional.bfttf", "4-korean.bfttf", "5-nintendo-extension.bfttf"};
static struct { PlatMutex lock; const uint8_t *file[SHARED_FONT_TYPES]; uint64_t size[SHARED_FONT_TYPES]; } fonts;

const char *ov_shared_font_dir(char *buf, size_t n) {
    const char *e = cfg_get("TOTK_SHARED_FONTS");
    if (e && *e) snprintf(buf, n, "%s", e);
    else {
        char exe[PLAT_PATH_MAX], probe[PLAT_PATH_MAX + 40];   // shared-fonts next to totk.exe (tools/package_build.py), else the repo's
        int len = plat_exe_path(exe, sizeof exe);
        char *cut = len && len < sizeof exe ? plat_last_sep(exe) : NULL;
        if (cut) *cut = 0;
        snprintf(probe, sizeof probe, "%s/shared-fonts/0-standard.bfttf", exe);
        if (cut && plat_file_exists(probe)) snprintf(buf, n, "%s/shared-fonts", exe);
        else snprintf(buf, n, "%s/../../firmware/shared-fonts", rt_config()->romfs);
    }
    return buf;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// The decoded font size of a BFTTF image, 0 if it is not one: big-endian words XORed with key = word0 ^ BFTTF_MAGIC,
// word1 = font size, then an sfnt (TrueType or OpenType) font (MNG extract_shared_fonts.py bfttf_size).
uint64_t ov_bfttf_size(const uint8_t *p, uint64_t len) {
    if (len < 12) return 0;
    uint32_t key = be32(p) ^ BFTTF_MAGIC, size = be32(p + 4) ^ key, first = be32(p + 8) ^ key;
    return (uint64_t)size + 8 == len && (first == 0x00010000u || first == 0x4f54544fu) ? size : 0;
}

static const char *font_type_name(int64_t t) { return font_files[t] + 2; }

static int64_t font_type(Cpu *c, const char *fn) {
    int64_t t = (int32_t)arg_x(c, 0);
    if (t < 0 || t >= SHARED_FONT_TYPES) rt_fatal("nn::pl::%s: invalid shared font type %lld", fn, (long long)t);
    return t;
}
static void ov_font_state(Cpu *c) { font_type(c, "GetSharedFontLoadState"); ret_x(c, 1); }

// The font of type t, read and checked on first use; a named fatal when it is missing or damaged.
static const uint8_t *font_data(Cpu *c, const char *fn, int64_t t) {
    plat_mutex_lock(&fonts.lock);
    if (!fonts.file[t]) {
        char dir[PLAT_PATH_MAX], path[PLAT_PATH_MAX + 64], lr[96];
        snprintf(path, sizeof path, "%s/%s", ov_shared_font_dir(dir, sizeof dir), font_files[t]);
        FILE *f = fopen(path, "rb");
        if (!f)
            rt_fatal("nn::pl::%s(%lld) from %s: no shared font %s (needed for Chinese and Korean text); run "
                     "tools/extract_fonts.py --firmware <firmware NCA dir> or set TOTK_SHARED_FONTS", fn, (long long)t,
                     caller(c, lr, sizeof lr), path);
        plat_fseek64(f, 0, SEEK_END);
        int64_t len = plat_ftell64(f);
        plat_fseek64(f, 0, SEEK_SET);
        if (len < 12 || len > (64 << 20)) rt_fatal("nn::pl: shared font %s: size %lld", path, (long long)len);
        uint8_t *p = mem_kernel_alloc((uint64_t)len, 16);
        size_t got = fread(p, 1, (size_t)len, f);
        fclose(f);
        uint64_t size = got == (size_t)len ? ov_bfttf_size(p, (uint64_t)len) : 0;
        if (!size) rt_fatal("nn::pl: shared font %s is not a BFTTF font", path);
        fonts.size[t] = size;
        __atomic_store_n(&fonts.file[t], p, __ATOMIC_RELEASE);
        rt_log("pl: shared font %lld (%s): %llu bytes from %s", (long long)t, font_type_name(t),
               (unsigned long long)size, path);
    }
    const uint8_t *p = fonts.file[t];
    plat_mutex_unlock(&fonts.lock);
    return p;
}
static void ov_font_address(Cpu *c) {
    int64_t t = font_type(c, "GetSharedFontAddress");
    ret_x(c, (uint64_t)(uintptr_t)(font_data(c, "GetSharedFontAddress", t) + 8));
}
static void ov_font_size(Cpu *c) {
    int64_t t = font_type(c, "GetSharedFontSize");
    font_data(c, "GetSharedFontSize", t);
    ret_x(c, fonts.size[t]);
}

// ==== registration
HOST_OVERRIDE("_ZN2nn2oe10InitializeEv", ov_oe_initialize);
HOST_OVERRIDE("_ZN2nn2oe18GetDesiredLanguageEv", ov_desired_language);
HOST_OVERRIDE("_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", ov_performance_config);
HOST_OVERRIDE("_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE", ov_cpu_boost);
HOST_OVERRIDE("_ZN2nn2oe16GetOperationModeEv", ov_operation_mode);
HOST_OVERRIDE("_ZN2nn2oe18GetPerformanceModeEv", ov_performance_mode);
HOST_OVERRIDE("_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", ov_focus_mode);
HOST_OVERRIDE("_ZN2nn2oe20GetCurrentFocusStateEv", ov_focus_state);
HOST_OVERRIDE("_ZN2nn2oe28SetResumeNotificationEnabledEb", ov_resume_enabled);
HOST_OVERRIDE("_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", ov_operation_enabled);
HOST_OVERRIDE("_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", ov_performance_enabled);

HOST_OVERRIDE("_ZN2nn6applet23GetPreviousProgramIndexEv", ov_previous_program);
HOST_OVERRIDE("_ZN2nn6applet23GetAppletResourceUserIdEv", ov_aruid);
HOST_OVERRIDE("_ZN2nn2am23GetAppletResourceUserIdEv", ov_aruid);
HOST_OVERRIDE("_ZN2nn2oe21TryPopLaunchParameterEPmPvm", ov_launch_parameter);
HOST_OVERRIDE("_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_", ov_get_balance);
HOST_OVERRIDE("_ZN2nn2oe24SetExpectedVolumeBalanceEff", ov_set_balance);
HOST_OVERRIDE("_ZN2nn2oe25GetProgramTotalActiveTimeEv", ov_active_time);
HOST_OVERRIDE("_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", ov_inactivity_extended);
HOST_OVERRIDE("_ZN2nn2oe44SetUserInactivityDetectionTimeExtendedUnsafeEb", ov_inactivity_extended);
HOST_OVERRIDE("_ZN2nn2oe17FinishStartupLogoEv", ov_finish_logo);
HOST_OVERRIDE("_ZN2nn2oe25TryPopNotificationMessageEPj", ov_try_pop);
HOST_OVERRIDE("_ZN2nn2oe22PopNotificationMessageEv", ov_pop);
HOST_OVERRIDE("_ZN2nn5prepo10PlayReport4SaveEv", ov_report_save);
HOST_OVERRIDE("_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE", ov_report_save);
HOST_OVERRIDE("_ZN2nn5prepo28RequestImmediateTransmissionEv", ov_result_ok);
HOST_OVERRIDE("_ZN2nn5prepo27AddInternetConnectionStatusEPNS0_10PlayReportEPKcS4_S4_", ov_result_ok);
HOST_OVERRIDE("_ZN2nn3nfp10InitializeEv", ov_nfp_initialize);
HOST_OVERRIDE("_ZN2nn3nfp8FinalizeEv", ov_nfp_finalize);
HOST_OVERRIDE("_ZN2nn3nfp8GetStateEv", ov_nfp_state);
HOST_OVERRIDE("_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", ov_nfp_list);
HOST_OVERRIDE("_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", ov_nfp_availability);
HOST_OVERRIDE("_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", ov_nfp_device_state);
HOST_OVERRIDE("_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", ov_nfp_no_device);
HOST_OVERRIDE("_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", ov_nfp_no_device);
HOST_OVERRIDE("_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", ov_nfp_no_device);
HOST_OVERRIDE("_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", ov_nfp_no_device);
HOST_OVERRIDE("_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", ov_nfp_no_device);
HOST_OVERRIDE("_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", ov_nfp_no_device);
HOST_OVERRIDE("_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", ov_nfp_no_device);
HOST_OVERRIDE("_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", ov_nfp_no_device);
HOST_OVERRIDE("_ZN2nn3err9ShowErrorENS_6ResultE", ov_show_error);

HOST_OVERRIDE("nndiagStartup", ov_diag_startup);
HOST_OVERRIDE("_ZN2nn3aoc10InitializeEv", ov_aoc_initialize);
HOST_OVERRIDE("_ZN2nn3aoc16ListAddOnContentEPiii", ov_aoc_list);
HOST_OVERRIDE("_ZN2nn3err26InitializeContextWriterApiEv", ov_err_context);
HOST_OVERRIDE("_ZN2nn4pctl10InitializeEv", ov_pctl_initialize);
HOST_OVERRIDE("_ZN2nn4diag34InitializeApplicationAbortObserverEv", ov_abort_observer);

HOST_OBSERVER("_ZN2nn2os21AllocateAddressRegionEPmm", obs_address_region);
HOST_OBSERVER("_ZN2nn2os19AllocateMemoryPagesEmm", obs_memory_pages);
HOST_OBSERVER("nnosInitialize", obs_os_initialize);
HOST_OBSERVER("nninitInitializeSdkModule", obs_sdk_initialize);
HOST_OBSERVER("nninitStartup", obs_startup);
HOST_OBSERVER("nnMain", obs_main);

HOST_OVERRIDE("_ZN2nn7account10InitializeEv", ov_account_initialize);
HOST_OVERRIDE("_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE", ov_open_user);
HOST_OVERRIDE("_ZN2nn7account9CloseUserERKNS0_10UserHandleE", ov_close_user);
HOST_OVERRIDE("_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE", ov_user_id);
HOST_OVERRIDE("_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE", ov_user_existence);
HOST_OVERRIDE("_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE", ov_nickname);

HOST_OVERRIDE("_ZN2nn4time10InitializeEv", ov_time_initialize);
HOST_OVERRIDE("_ZN2nn4time8FinalizeEv", ov_time_finalize);
HOST_OVERRIDE("_ZN2nn4time13IsInitializedEv", ov_time_ready);
HOST_OVERRIDE("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", ov_current_time);
HOST_OVERRIDE("_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", ov_current_time);
HOST_OVERRIDE("_ZN2nn4time19StandardSteadyClock3nowEv", ov_steady_now);
HOST_OVERRIDE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE",
              ov_to_calendar);
HOST_OVERRIDE("_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE", ov_to_calendar_utc);
HOST_OVERRIDE("_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE", ov_to_posix_utc);
HOST_OVERRIDE("_ZN2nn4time13ClockSnapshotC1Ev", ov_snapshot_init);
HOST_OVERRIDE("_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_", ov_snapshot_create);
HOST_OVERRIDE("_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_", ov_span_between);
HOST_OVERRIDE("_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_",
              ov_user_difference);

HOST_OVERRIDE("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", ov_font_state);
HOST_OVERRIDE("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", ov_font_address);
HOST_OVERRIDE("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", ov_font_size);
