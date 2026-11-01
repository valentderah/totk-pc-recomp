// --probe-services: every override of ov_system.c, ov_input.c, ov_media.c, swkbd.c and
// ov_gfx.c called the way guest code calls it: by its synthetic address through rt_call / rt_dispatch, with AAPCS64
// arguments, on the guest main thread. Checks that each one matches a relocation of the loaded modules, then the
// values: language, operation mode, notification messages, account, time round trips, controllers, codec state,
// graphics records, the system events behind nfp and vi, the software keyboard with arguments built by the guest SDK
// (answers from TOTK_SWKBD_ANSWER, then the window pressed by TOTK_SWKBD_TEST) and the shared fonts when
// tools/extract_fonts.py has written them. The named fatal stubs, and the account UID of a second run, are checked in
// child processes (this executable again, TOTK_PROBE_SERVICES_CHILD=<case>).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfg.h"
#include "fs.h"
#include "gfx.h"
#include "host.h"
#include "input.h"
#include "kernel.h"
#include "ov.h"
#include "platform/platform.h"
#include "utf16.h"
#include "ui_text.h"

#define CHILD_ENV "TOTK_PROBE_SERVICES_CHILD"
#define TRY_WAIT_SYSTEM_EVENT "_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE"

typedef struct { const char *key, *name; int fatal; uint64_t addr; int bound, calls; } Ov;
static Ov ovs[] = {
    // ov_system.c: nn::oe
    {"oe.Initialize", "_ZN2nn2oe10InitializeEv"},
    {"oe.GetDesiredLanguage", "_ZN2nn2oe18GetDesiredLanguageEv"},
    {"oe.SetPerformanceConfiguration", "_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi"},
    {"oe.SetCpuBoostMode", "_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE"},
    {"oe.GetOperationMode", "_ZN2nn2oe16GetOperationModeEv"},
    {"oe.GetPerformanceMode", "_ZN2nn2oe18GetPerformanceModeEv"},
    {"oe.SetFocusHandlingMode", "_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE"},
    {"oe.GetCurrentFocusState", "_ZN2nn2oe20GetCurrentFocusStateEv"},
    {"oe.SetResumeNotificationEnabled", "_ZN2nn2oe28SetResumeNotificationEnabledEb"},
    {"oe.SetOperationModeChangedNotificationEnabled", "_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb"},
    {"oe.SetPerformanceModeChangedNotificationEnabled", "_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb"},
    // applet, am, oe (applet_host.c)
    {"applet.GetPreviousProgramIndex", "_ZN2nn6applet23GetPreviousProgramIndexEv"},
    {"applet.GetAppletResourceUserId", "_ZN2nn6applet23GetAppletResourceUserIdEv"},
    {"am.GetAppletResourceUserId", "_ZN2nn2am23GetAppletResourceUserIdEv"},
    {"oe.TryPopLaunchParameter", "_ZN2nn2oe21TryPopLaunchParameterEPmPvm"},
    {"oe.GetExpectedVolumeBalance", "_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_"},
    {"oe.SetExpectedVolumeBalance", "_ZN2nn2oe24SetExpectedVolumeBalanceEff"},
    {"oe.GetProgramTotalActiveTime", "_ZN2nn2oe25GetProgramTotalActiveTimeEv"},
    {"oe.SetUserInactivityDetectionTimeExtended", "_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb"},
    {"oe.SetUserInactivityDetectionTimeExtendedUnsafe", "_ZN2nn2oe44SetUserInactivityDetectionTimeExtendedUnsafeEb"},
    {"oe.FinishStartupLogo", "_ZN2nn2oe17FinishStartupLogoEv"},
    {"oe.TryPopNotificationMessage", "_ZN2nn2oe25TryPopNotificationMessageEPj"},
    {"oe.PopNotificationMessage", "_ZN2nn2oe22PopNotificationMessageEv"},
    {"prepo.Save", "_ZN2nn5prepo10PlayReport4SaveEv"},
    {"prepo.SaveUid", "_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE"},
    {"prepo.RequestImmediateTransmission", "_ZN2nn5prepo28RequestImmediateTransmissionEv"},
    {"prepo.AddInternetConnectionStatus", "_ZN2nn5prepo27AddInternetConnectionStatusEPNS0_10PlayReportEPKcS4_S4_"},
    {"nfp.Initialize", "_ZN2nn3nfp10InitializeEv"},
    {"nfp.Finalize", "_ZN2nn3nfp8FinalizeEv"},
    {"nfp.GetState", "_ZN2nn3nfp8GetStateEv"},
    {"nfp.ListDevices", "_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii"},
    {"nfp.AttachAvailabilityChangeEvent", "_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE"},
    {"nfp.GetDeviceState", "_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE"},
    {"nfp.Mount", "_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE"},
    {"nfp.GetNpadId", "_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE"},
    {"nfp.AttachActivateEvent", "_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE"},
    {"nfp.AttachDeactivateEvent", "_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE"},
    {"nfp.StartDetection", "_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE"},
    {"nfp.StopDetection", "_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE"},
    {"nfp.GetTagInfo", "_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE"},
    {"nfp.GetModelInfo", "_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE"},
    {"err.ShowError", "_ZN2nn3err9ShowErrorENS_6ResultE"},
    // SDK-internal (section 5.2)
    {"nndiagStartup", "nndiagStartup"},
    {"aoc.Initialize", "_ZN2nn3aoc10InitializeEv"},
    {"aoc.ListAddOnContent", "_ZN2nn3aoc16ListAddOnContentEPiii"},
    {"err.InitializeContextWriterApi", "_ZN2nn3err26InitializeContextWriterApiEv"},
    {"pctl.Initialize", "_ZN2nn4pctl10InitializeEv"},
    {"diag.InitializeApplicationAbortObserver", "_ZN2nn4diag34InitializeApplicationAbortObserverEv"},
    // account
    {"account.Initialize", "_ZN2nn7account10InitializeEv"},
    {"account.TryOpenPreselectedUser", "_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE"},
    {"account.CloseUser", "_ZN2nn7account9CloseUserERKNS0_10UserHandleE"},
    {"account.GetUserId", "_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE"},
    {"account.GetUserExistence", "_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE"},
    {"account.GetNickname", "_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE"},
    // time
    {"time.Initialize", "_ZN2nn4time10InitializeEv"},
    {"time.Finalize", "_ZN2nn4time8FinalizeEv"},
    {"time.IsInitialized", "_ZN2nn4time13IsInitializedEv"},
    {"time.UserClock.GetCurrentTime", "_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE"},
    {"time.NetworkClock.GetCurrentTime", "_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE"},
    {"time.SteadyClock.now", "_ZN2nn4time19StandardSteadyClock3nowEv"},
    {"time.ToCalendarTime",
     "_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE"},
    {"time.ToCalendarTimeInUtc", "_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE"},
    {"time.ToPosixTimeFromUtc", "_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE"},
    {"time.ClockSnapshot.C1", "_ZN2nn4time13ClockSnapshotC1Ev"},
    {"time.ClockSnapshot.Create", "_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_"},
    {"time.CalculateSpanBetween", "_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_"},
    {"time.UserDifference",
     "_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_"},
    // pl
    {"pl.GetSharedFontLoadState", "_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE"},
    {"pl.GetSharedFontAddress", "_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", 1},
    {"pl.GetSharedFontSize", "_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", 1},
    // ov_input.c
    {"hid.InitializeNpad", "_ZN2nn3hid14InitializeNpadEv"},
    {"hid.SetSupportedNpadIdType", "_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm"},
    {"hid.SetSupportedNpadStyleSet",
     "_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE"},
    {"hid.GetNpadStyleSet", "_ZN2nn3hid15GetNpadStyleSetERKj"},
    {"hid.GetNpadStates.FullKey", "_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj"},
    {"hid.GetNpadStates.JoyDual", "_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj"},
    {"hid.GetNpadStates.JoyLeft", "_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj"},
    {"hid.GetNpadStates.JoyRight", "_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj"},
    {"hid.GetNpadStates.Handheld", "_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj"},
    {"hid.SetNpadJoyHoldType", "_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE"},
    {"hid.DisconnectNpad", "_ZN2nn3hid14DisconnectNpadERKj"},
    {"hid.ShowControllerSupport",
     "_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE"},
    {"hid.InitializeMouse", "_ZN2nn3hid15InitializeMouseEv"},
    {"hid.InitializeKeyboard", "_ZN2nn3hid18InitializeKeyboardEv"},
    {"hid.GetMouseStates", "_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi"},
    {"hid.GetKeyboardState", "_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE"},
    {"hid.GetVibrationDeviceHandles",
     "_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_"
     "12NpadStyleTagEEE"},
    {"hid.GetSixAxisSensorHandles",
     "_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_"
     "12NpadStyleTagEEE"},
    {"hid.InitializeVibrationDevice", "_ZN2nn3hid6detail25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE"},
    {"hid.SendVibrationValue",
     "_ZN2nn3hid6detail18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE"},
    {"hid.SendVibrationValues",
     "_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi"},
    {"hid.GetActualVibrationValue",
     "_ZN2nn3hid6detail23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE"},
    {"hid.GetVibrationDeviceInfo",
     "_ZN2nn3hid6detail22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE"},
    {"hid.IsVibrationDeviceMounted", "_ZN2nn3hid6detail24IsVibrationDeviceMountedEPbRKNS0_21VibrationDeviceHandleE"},
    {"hid.IsVibrationPermitted", "_ZN2nn3hid6detail20IsVibrationPermittedEPb"},
    {"hid.PermitVibration", "_ZN2nn3hid6detail15PermitVibrationEb"},
    {"hid.StartSixAxisSensor", "_ZN2nn3hid6detail18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE"},
    {"hid.StopSixAxisSensor", "_ZN2nn3hid6detail17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE"},
    {"hid.GetSixAxisSensorStates",
     "_ZN2nn3hid6detail22GetSixAxisSensorStatesEPiPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE"},
    {"hid.GetSixAxisSensorState",
     "_ZN2nn3hid6detail21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE"},
    // ov_media.c
    {"codec.GetWorkBufferSize", "_ZN2nn5codec36GetHardwareOpusDecoderWorkBufferSizeEiii"},
    {"codec.Initialize", "_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiPvmi"},
    {"codec.Finalize", "_ZN2nn5codec27FinalizeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE"},
    {"codec.GetSampleRate", "_ZN2nn5codec32GetHardwareOpusDecoderSampleRateEPKNS0_23HardwareOpusDecoderTypeE"},
    {"codec.GetChannelCount", "_ZN2nn5codec34GetHardwareOpusDecoderChannelCountEPKNS0_23HardwareOpusDecoderTypeE"},
    {"codec.Reset", "_ZN2nn5codec24ResetHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE"},
    {"codec.Decode", "_ZN2nn5codec33DecodeOpusInterleavedWithHardwareEPNS0_23HardwareOpusDecoderTypeEPmPiPsmPKvm", 1},
    {"swkbd.ShowKeyboard", "_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", 1},
    // ov_gfx.c
    {"nv.SetGraphicsAllocator", "_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_"},
    {"nv.InitializeGraphics", "_ZN2nv18InitializeGraphicsEPvm"},
    {"nv.SetGraphicsDevtoolsAllocator", "_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_"},
    {"vi.Initialize", "_ZN2nn2vi10InitializeEv"},
    {"vi.OpenDefaultDisplay", "_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE"},
    {"vi.CreateLayer", "_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE"},
    {"vi.GetNativeWindow", "_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE"},
    {"vi.GetDisplayVsyncEvent", "_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE"},
    {"nvnBootstrapLoader", "nvnBootstrapLoader", 1},
};
#define NOV ((int)(sizeof ovs / sizeof *ovs))

static const char *const observers[] = {"_ZN2nn2os21AllocateAddressRegionEPmm", "_ZN2nn2os19AllocateMemoryPagesEmm",
                                        "nnosInitialize", "nninitInitializeSdkModule", "nninitStartup", "nnMain"};
#define NOBS ((int)(sizeof observers / sizeof *observers))

// Fatal stubs, each in its own child process: the case name, the override, what its FATAL line must say and an
// environment variable the child runs with (the shared fonts: a font directory that does not exist).
#define NO_FONTS "TOTK_SHARED_FONTS=:probe-services:no-such-dir"
static const struct { const char *child, *key, *fatal, *env; } fatals[] = {
    {"opus", "codec.Decode", "FATAL: nn::codec::DecodeOpusInterleavedWithHardware"},
    {"font-address", "pl.GetSharedFontAddress", "FATAL: nn::pl::GetSharedFontAddress(0)", NO_FONTS},
    {"font-size", "pl.GetSharedFontSize", "FATAL: nn::pl::GetSharedFontSize(0)", NO_FONTS},
};
#define NFATAL ((int)(sizeof fatals / sizeof *fatals))

static int failures, checks;
#define CHECK(cond, ...) \
    do { checks++; if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

static Cpu *gc;            // the guest thread the calls run on
static uint64_t probe_uid[2];

static Ov *ov(const char *key) {
    for (int i = 0; i < NOV; i++)
        if (!strcmp(ovs[i].key, key)) return &ovs[i];
    rt_fatal("probe-services: no case %s", key);
}

static uint64_t U(const void *p) { return (uint64_t)(uintptr_t)p; }

// The override called through its synthetic address with x0..x7 = a[0..7]; caller-saved registers hold garbage, the
// callee-saved ones a pattern that must survive. Returns x0.
static uint64_t call8(const char *key, const uint64_t a[8]) {
    Ov *o = ov(key);
    for (int r = 8; r < 19; r++) gc->x[r] = 0xdead0000u + r;
    for (int r = 19; r < 30; r++) gc->x[r] = 0x1900u + r;
    uint64_t sp = gc->sp;
    uint64_t v = rt_call(gc, o->addr, 8, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    int kept = gc->sp == sp;
    for (int r = 19; r < 30; r++) kept &= gc->x[r] == 0x1900u + r;
    CHECK(kept, "%s lost a callee-saved register or sp", key);
    o->calls++;
    return v;
}
#define CALL(key, ...) call8(key, (const uint64_t[8]){__VA_ARGS__})

static void set_s(int i, float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    gc->v[i] = b;
}

static void bind_addresses(void) {
    for (int i = 0; i < NOV; i++) {
        GuestFn fn = hostfn_override(ovs[i].name);
        if (!fn) rt_fatal("probe-services: no HOST_OVERRIDE for %s", ovs[i].name);
        ovs[i].addr = hostfn_register(ovs[i].name, fn);
    }
}

static void *guest_alloc(uint64_t n) { return mem_kernel_alloc(n, 16); }

// ---- the guest-thread part
static char *lang(void) {
    static char s[9];
    uint64_t v = CALL("oe.GetDesiredLanguage", 0);
    memcpy(s, &v, 8);
    s[8] = 0;
    return s;
}

static void set_env(const char *name, const char *value) { plat_setenv(name, value); }   // NULL removes it

static void probe_oe(void) {
    char *keep_lang = cfg_get("TOTK_LANG") ? strdup(cfg_get("TOTK_LANG")) : NULL;
    char *keep_locale = cfg_get("TOTK_LOCALE") ? strdup(cfg_get("TOTK_LOCALE")) : NULL;
    static const struct { const char *lang, *locale, *want; } cases[] = {
        {NULL, "en-US", "en-US"}, {NULL, "be-BY", "en-US"}, {NULL, "xx", "en-US"},     {NULL, "ja-JP", "ja"},
        {NULL, "fr-CA", "fr-CA"}, {NULL, "es-MX", "es-419"}, {NULL, "es_ES", "es"},     {NULL, "zh-Hant-TW", "zh-Hant"},
        {NULL, "zh-CN", "zh-Hans"}, {NULL, "en-GB", "en-GB"}, {NULL, "ru", "ru"},       {"ja", "fr-FR", "ja"},
        {"en-US", NULL, "en-US"}, {"zh-CN", NULL, "zh-Hans"}, {"zh-TW", NULL, "zh-Hant"},
        {"zh-Hans", NULL, "zh-Hans"}, {"zh-Hant", NULL, "zh-Hant"}};
    int bad = 0;
    printf("oe: configured language (TOTK_LANG=%s) is %s\n", keep_lang ? keep_lang : "", lang());
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        set_env("TOTK_LANG", cases[i].lang);
        set_env("TOTK_LOCALE", cases[i].locale);
        const char *got = lang();
        if (strcmp(got, cases[i].want)) {
            bad++;
            printf("  FAIL: language for TOTK_LANG=%s TOTK_LOCALE=%s is %s, not %s\n", cases[i].lang ? cases[i].lang : "",
                   cases[i].locale ? cases[i].locale : "", got, cases[i].want);
        }
    }
    set_env("TOTK_LANG", NULL);
    set_env("TOTK_LOCALE", NULL);
    char sys[16];
    snprintf(sys, sizeof sys, "%s", lang());
    set_env("TOTK_LOCALE", "en-US");
    char def[16];
    snprintf(def, sizeof def, "%s", lang());
    set_env("TOTK_LANG", keep_lang);
    set_env("TOTK_LOCALE", keep_locale);
    free(keep_lang);
    free(keep_locale);
    checks++;
    failures += bad > 0;
    printf("oe: language %s by default (en-US locale), %s for this user's locale; %d locale/TOTK_LANG cases %s\n",
           def, sys, (int)(sizeof cases / sizeof *cases), bad ? "FAILED" : "ok");

    CALL("oe.Initialize", 0);
    CALL("oe.SetPerformanceConfiguration", 1, 0x92220008);
    CALL("oe.SetCpuBoostMode", 0);
    uint64_t mode = CALL("oe.GetOperationMode", 0), perf = CALL("oe.GetPerformanceMode", 0);
    CHECK((uint32_t)mode == (uint32_t)!ov_handheld() && (uint32_t)perf == (uint32_t)!ov_handheld(),
          "operation/performance mode %llu/%llu", (unsigned long long)mode, (unsigned long long)perf);
    CALL("oe.SetFocusHandlingMode", 1);
    CHECK((uint32_t)CALL("oe.GetCurrentFocusState", 0) == 1, "focus state");
    CALL("oe.SetResumeNotificationEnabled", 1);
    CALL("oe.SetOperationModeChangedNotificationEnabled", 1);
    CALL("oe.SetPerformanceModeChangedNotificationEnabled", 1);
    CHECK((int32_t)CALL("applet.GetPreviousProgramIndex", 0) == -1, "previous program index");
    CHECK(CALL("applet.GetAppletResourceUserId", 0) == 1 && CALL("am.GetAppletResourceUserId", 0) == 1, "ARUID");
    uint64_t size = 99;
    uint8_t param[16];
    CHECK((uint8_t)CALL("oe.TryPopLaunchParameter", U(&size), U(param), sizeof param) == 0 && size == 0,
          "launch parameter");
    float l = 0, r = 0;
    set_s(0, 0.25f);
    set_s(1, 0.75f);
    CALL("oe.SetExpectedVolumeBalance", 0);
    CALL("oe.GetExpectedVolumeBalance", U(&l), U(&r));
    CHECK(l == 0.25f && r == 0.75f, "volume balance %g/%g", (double)l, (double)r);
    int64_t t1 = (int64_t)CALL("oe.GetProgramTotalActiveTime", 0);
    plat_sleep_ms(2);
    int64_t t2 = (int64_t)CALL("oe.GetProgramTotalActiveTime", 0);
    CHECK(t1 > 0 && t2 > t1, "program total active time %lld, %lld", (long long)t1, (long long)t2);
    CALL("oe.SetUserInactivityDetectionTimeExtended", 1);
    CALL("oe.SetUserInactivityDetectionTimeExtendedUnsafe", 1);
    CALL("oe.FinishStartupLogo", 0);
}

static void late_push(void *p) {
    (void)p;
    plat_sleep_ms(50);
    oe_push_message(0x21);
    return;
}

static void probe_messages(void) {
    uint32_t m = 0;
    CHECK((uint8_t)CALL("oe.TryPopNotificationMessage", U(&m)) == 0, "a message before any was pushed");
    oe_push_message(0x1f);
    oe_push_message(0x20);
    CHECK((uint8_t)CALL("oe.TryPopNotificationMessage", U(&m)) == 1 && m == 0x1f, "TryPop after a push: %#x", m);
    CHECK((uint32_t)CALL("oe.PopNotificationMessage", 0) == 0x20, "Pop after a push");
    PlatThread *h = plat_thread_new(late_push, NULL, "late push");
    uint64_t t0 = kern_now_ns();
    uint32_t late = (uint32_t)CALL("oe.PopNotificationMessage", 0);
    uint64_t waited = kern_now_ns() - t0;
    plat_thread_join(h);
    CHECK(late == 0x21 && waited >= 40000000, "blocking Pop got %#x after %llu ms", late,
          (unsigned long long)(waited / 1000000));
    printf("oe: notification messages pushed then popped (TryPop, Pop, Pop blocked %llu ms until a host push): %s\n",
           (unsigned long long)(waited / 1000000), late == 0x21 ? "ok" : "FAILED");
}

// A system event bound by an override: not signalled, then signalled through the kernel, then (auto-clear) cleared.
static int event_works(uint64_t ev, int autoclear) {
    static uint64_t try_wait;
    if (!try_wait && !(try_wait = ldr_symbol(TRY_WAIT_SYSTEM_EVENT))) rt_fatal("probe-services: no %s", TRY_WAIT_SYSTEM_EVENT);
    int before = (uint8_t)rt_call(gc, try_wait, 1, ev);
    kern_signal_system_event(ev);
    int after = (uint8_t)rt_call(gc, try_wait, 1, ev);
    int again = (uint8_t)rt_call(gc, try_wait, 1, ev);
    return !before && after && again == !autoclear;
}

static void probe_applet(void) {
    uint8_t *report = guest_alloc(0x100);
    uint64_t uid[2] = {1, 2};
    CHECK(CALL("prepo.Save", U(report)) == 0 && CALL("prepo.SaveUid", U(report), U(uid)) == 0, "PlayReport::Save");
    CHECK(CALL("prepo.RequestImmediateTransmission", 0) == 0, "RequestImmediateTransmission");
    CHECK(CALL("prepo.AddInternetConnectionStatus", U(report), U("a"), U("b"), U("c")) == 0,
          "AddInternetConnectionStatus");
    CHECK(CALL("nfp.Initialize", 0) == 0 && (uint32_t)CALL("nfp.GetState", 0) == 1, "nfp initialize");
    uint8_t handles[64];
    int32_t count = 7;
    CHECK((uint32_t)CALL("nfp.ListDevices", U(handles), U(&count), 4) == 0x8073 && count == 0, "nfp ListDevices");
    uint64_t ev = U(guest_alloc(0x100));
    CHECK(CALL("nfp.AttachAvailabilityChangeEvent", ev) == 0, "AttachAvailabilityChangeEvent");
    int nfp_event = event_works(ev, 0);
    CHECK(nfp_event, "the nfp availability event does not work as a manual-clear system event");
    CHECK((uint32_t)CALL("nfp.GetDeviceState", U(handles)) == 0, "nfp device state");
    static const char *const none[] = {"nfp.Mount", "nfp.GetNpadId", "nfp.AttachActivateEvent", "nfp.AttachDeactivateEvent",
                                       "nfp.StartDetection", "nfp.StopDetection", "nfp.GetTagInfo", "nfp.GetModelInfo"};
    uint8_t out[0x100];
    for (size_t i = 0; i < sizeof none / sizeof *none; i++) {
        int first_is_handle = !strcmp(none[i], "nfp.Mount") || strstr(none[i], "Detection");
        uint64_t r = first_is_handle ? CALL(none[i], U(handles), 0, 0) : CALL(none[i], U(out), U(handles));
        CHECK((uint32_t)r == 0x8073, "%s: %#x, not device-not-found", none[i], (uint32_t)r);
    }
    CHECK(CALL("nfp.Finalize", 0) == 0 && (uint32_t)CALL("nfp.GetState", 0) == 0, "nfp finalize");
    CALL("err.ShowError", 0x2a8);
    CALL("nndiagStartup", 0);
    CALL("aoc.Initialize", 0);
    CHECK(CALL("aoc.ListAddOnContent", U(out), 0, 8) == 0, "aoc: no add-on content");
    CALL("err.InitializeContextWriterApi", 0);
    CALL("pctl.Initialize", 0);
    CALL("diag.InitializeApplicationAbortObserver", 0);
    printf("applet/prepo/nfp/err: launch parameter none, volume balance, play reports local, no NFC device, "
           "availability event (bound through the SDK's CreateSystemEvent) %s\n", nfp_event ? "ok" : "FAILED");
}

static void probe_account(void) {
    uint64_t h[3] = {0}, uid[2] = {0}, other[2] = {5, 6}, direct[2];
    uint8_t exists = 0;
    char nick[33];
    CALL("account.Initialize", 0);
    CHECK((uint8_t)CALL("account.TryOpenPreselectedUser", U(h)) == 1, "TryOpenPreselectedUser");
    CHECK(CALL("account.GetUserId", U(uid), U(h)) == 0 && (uid[0] || uid[1]), "GetUserId");
    account_uid(direct);
    CHECK(uid[0] == direct[0] && uid[1] == direct[1], "GetUserId differs from account_uid");
    CHECK(CALL("account.GetUserExistence", U(&exists), U(uid)) == 0 && exists == 1, "GetUserExistence");
    CHECK(CALL("account.GetUserExistence", U(&exists), U(other)) == 0 && exists == 0, "GetUserExistence (other)");
    CHECK(CALL("account.GetNickname", U(nick), U(uid)) == 0 && !strcmp(nick, "Local"), "GetNickname");
    CALL("account.CloseUser", U(h));
    CHECK(CALL("account.GetUserId", U(uid), U(h)) != 0, "GetUserId on a closed handle");
    probe_uid[0] = direct[0];
    probe_uid[1] = direct[1];
}

typedef struct { int16_t year; int8_t month, day, hour, minute, second, pad; } Cal;
typedef struct { int32_t day_of_week, day_of_year; char name[8]; uint8_t dst, pad[3]; int32_t utc_offset; } CalInfo;

static int64_t posix_of(const Cal *c) { return (int64_t)CALL("time.ToPosixTimeFromUtc", U(c)); }
static Cal utc_of(int64_t t) {
    uint64_t v = CALL("time.ToCalendarTimeInUtc", U(&t));
    Cal c;
    memcpy(&c, &v, 8);
    return c;
}

static void probe_time(void) {
    CHECK(CALL("time.Initialize", 0) == 0 && (uint8_t)CALL("time.IsInitialized", 0) == 1, "time initialize");
    int64_t user = 0, net = 0, now = (int64_t)time(NULL);
    CHECK(CALL("time.UserClock.GetCurrentTime", U(&user)) == 0 && CALL("time.NetworkClock.GetCurrentTime", U(&net)) == 0 &&
          llabs(user - now) <= 2 && llabs(net - now) <= 2, "system clocks %lld/%lld vs %lld", (long long)user,
          (long long)net, (long long)now);
    int64_t s1 = (int64_t)CALL("time.SteadyClock.now", 0), s2 = (int64_t)CALL("time.SteadyClock.now", 0);
    CHECK(s1 > 0 && s2 >= s1, "steady clock %lld, %lld", (long long)s1, (long long)s2);

    static const int64_t ts[] = {0, 86399, 951782400, 1700000000, 4102444800ll, -31536000, -62135596800ll, 253402300799ll};
    int rt_bad = 0;
    for (size_t i = 0; i < sizeof ts / sizeof *ts; i++) {
        Cal c = utc_of(ts[i]);
        if (posix_of(&c) != ts[i]) rt_bad++;
    }
    Cal leap = utc_of(951782400);   // 2000-02-29 00:00:00 UTC
    CHECK(leap.year == 2000 && leap.month == 2 && leap.day == 29 && !leap.hour && !leap.minute && !leap.second,
          "951782400 is %d-%d-%d", leap.year, leap.month, leap.day);
    Cal carry = {2023, 13, 1, 0, 0, 0, 0};   // month 13 carries into the next year
    CHECK(posix_of(&carry) == 1704067200, "ToPosixTimeFromUtc does not carry month 13");
    CHECK(!rt_bad, "%d of %d UTC round trips failed", rt_bad, (int)(sizeof ts / sizeof *ts));

    Cal local = {0};
    CalInfo info = {0};
    int64_t t = 1700000000;
    CHECK(CALL("time.ToCalendarTime", U(&local), U(&info), U(&t)) == 0, "ToCalendarTime");
    int64_t back = posix_of(&local);
    int64_t day = (t + info.utc_offset) / 86400;
    CHECK(back - t == info.utc_offset && info.day_of_week == (int32_t)((day + 4) % 7),
          "local calendar time does not round trip (offset %d, day of week %d)", info.utc_offset, info.day_of_week);
    int64_t neg = -1;
    CHECK(CALL("time.ToCalendarTime", U(&local), U(&info), U(&neg)) != 0, "ToCalendarTime of an invalid time succeeded");

    uint8_t *a = guest_alloc(0xd0), *b = guest_alloc(0xd0);
    memset(a, 0xaa, 0xd0);
    CALL("time.ClockSnapshot.C1", U(a));
    int zero = 1;
    for (int i = 0; i < 0xd0; i++) zero &= !a[i];
    CHECK(zero, "ClockSnapshot() did not clear the snapshot");
    CHECK(CALL("time.ClockSnapshot.Create", U(a)) == 0 && CALL("time.ClockSnapshot.Create", U(b)) == 0, "snapshots");
    int64_t span = -1;
    CHECK(CALL("time.CalculateSpanBetween", U(&span), U(a), U(b)) == 0 && span >= 0, "span between snapshots");
    CHECK(CALL("time.UserDifference", U(a), U(b)) == 0, "user clock difference");
    b[0x98] ^= 1;   // the steady clock's source id (ClockSnapshot +0x90: value, +0x98: source)
    CHECK((uint32_t)CALL("time.CalculateSpanBetween", U(&span), U(a), U(b)) == 0x18074u, "span across sources");
    CHECK(CALL("time.Finalize", 0) == 0 && (uint8_t)CALL("time.IsInitialized", 0) == 0, "time finalize");
    printf("time: ToCalendarTimeInUtc/ToPosixTimeFromUtc round trip %d values %s; local %04d-%02d-%02d %02d:%02d zone %s "
           "offset %+d s; snapshot location %s\n", (int)(sizeof ts / sizeof *ts), rt_bad ? "FAILED" : "ok", local.year,
           local.month, local.day, local.hour, local.minute, info.name, info.utc_offset, (const char *)(a + 0xa8));
}

#define STD_GRAVITY 9.80665f   // SDL_STANDARD_GRAVITY

static int near3(const float *v, double x, double y, double z, double tol) {
    return fabs(v[0] - x) <= tol && fabs(v[1] - y) <= tol && fabs(v[2] - z) <= tol;
}
static int at_rest(const MotionState *s) {
    return !s->attributes && !s->sampling && near3(s->acc, 0, 0, -1, 0) && near3(s->gyro, 0, 0, 0, 0) &&
           near3(s->dir, 1, 0, 0, 0) && near3(s->dir + 3, 0, 1, 0, 0) && near3(s->dir + 6, 0, 0, 1, 0);
}

// Vibration and six-axis handles, values and states with no gamepad motion (the sensor is stopped before the reads that
// check the rest state), the SDL -> Switch axis mapping and the sensor fusion on a private instance.
static void probe_feedback(uint32_t pid, uint32_t style) {
    uint32_t six = ov_handheld() ? 4 : 3, vh[2] = {0}, sh[2] = {0}, dual[2] = {0}, zero = 0;
    CHECK((int32_t)CALL("hid.GetVibrationDeviceHandles", U(vh), 2, U(&pid), style) == 2 && vh[0] == (six | pid << 8) &&
          vh[1] == (0x10000 | six | pid << 8), "vibration handles %#x %#x", vh[0], vh[1]);
    CHECK((int32_t)CALL("hid.GetSixAxisSensorHandles", U(sh), 2, U(&pid), style) == 1 && sh[0] == (0x20000 | six | pid << 8),
          "six-axis handle %#x", sh[0]);
    CHECK((int32_t)CALL("hid.GetVibrationDeviceHandles", U(dual), 2, U(&zero), 4) == 2 && dual[0] == 5 &&
          dual[1] == 0x10005, "Joy-Con pair vibration handles %#x %#x (SDK: 0x5 0x10005)", dual[0], dual[1]);
    char *keep_r = cfg_get("TOTK_RUMBLE") ? strdup(cfg_get("TOTK_RUMBLE")) : NULL;
    char *keep_m = cfg_get("TOTK_MOTION") ? strdup(cfg_get("TOTK_MOTION")) : NULL;
    set_env("TOTK_RUMBLE", "0");
    set_env("TOTK_MOTION", "0");
    uint32_t none[2] = {0xdead, 0xdead}, six_off[2] = {0};
    CHECK((int32_t)CALL("hid.GetVibrationDeviceHandles", U(none), 2, U(&pid), style) == 0 && none[0] == 0xdead,
          "TOTK_RUMBLE=0 still reports vibration handles");
    // TOTK_MOTION is live: the handle stays (the sensor reads at rest while it is 0)
    CHECK((int32_t)CALL("hid.GetSixAxisSensorHandles", U(six_off), 2, U(&pid), style) == 1 && six_off[0] == sh[0],
          "TOTK_MOTION=0 drops the six-axis handle %#x", six_off[0]);
    set_env("TOTK_RUMBLE", keep_r);
    set_env("TOTK_MOTION", keep_m);
    free(keep_r);
    free(keep_m);

    // Values with no amplitude: a connected gamepad stays still
    float v[2][4] = {{0, 100, 0, 400}, {0, 150, 0, 300}}, got[4] = {0};
    uint32_t info[2] = {9, 9};
    uint8_t flag = 0;
    CHECK(CALL("hid.InitializeVibrationDevice", U(&vh[0])) == 0 && CALL("hid.InitializeVibrationDevice", U(&vh[1])) == 0,
          "InitializeVibrationDevice");
    CHECK(CALL("hid.SendVibrationValue", U(&vh[0]), U(v[0])) == 0 && CALL("hid.GetActualVibrationValue", U(got), U(&vh[0])) == 0 &&
          !memcmp(got, v[0], sizeof got), "the actual value is not the one sent");
    CHECK(CALL("hid.SendVibrationValues", U(vh), U(v), 2) == 0 && CALL("hid.GetActualVibrationValue", U(got), U(&vh[1])) == 0 &&
          !memcmp(got, v[1], sizeof got), "a batch does not reach the right device");
    CHECK(CALL("hid.GetVibrationDeviceInfo", U(info), U(&vh[1])) == 0 && info[0] == 1 && info[1] == 2,
          "right device info %u/%u", info[0], info[1]);
    CHECK(CALL("hid.IsVibrationDeviceMounted", U(&flag), U(&vh[0])) == 0 && flag == 1 &&
          CALL("hid.IsVibrationDeviceMounted", U(&flag), U(&dual[0])) == 0 && flag == 0, "mounted devices");
    CHECK(CALL("hid.PermitVibration", 1) == 0 && CALL("hid.IsVibrationPermitted", U(&flag)) == 0 && flag == 1,
          "vibration permitted");

    MotionState *st = guest_alloc(17 * sizeof *st);
    memset(st, 0xa5, 17 * sizeof *st);
    int32_t count = -1;
    CALL("hid.StartSixAxisSensor", U(&sh[0]));
    CALL("hid.StopSixAxisSensor", U(&sh[0]));
    CHECK(CALL("hid.GetSixAxisSensorStates", U(&count), U(st), 16, U(&sh[0])) == 0 && count == 16, "six-axis count %d",
          count);
    int rest = 1, guard = 1;
    for (int i = 0; i < 16; i++) rest &= at_rest(&st[i]);
    for (size_t i = 0; i < sizeof *st; i++) guard &= ((uint8_t *)&st[16])[i] == 0xa5;
    CHECK(rest && guard, "stopped sensor: 16 states at rest %d, guard kept %d", rest, guard);
    memset(st, 0xa5, sizeof *st);
    CHECK(CALL("hid.GetSixAxisSensorState", U(st), U(&dual[0])) == 0 && at_rest(st), "a foreign handle reads at rest");

    // SDL axes: lying flat (push +y) is Switch gravity -z, standing on the bottom edge is -y; yaw about SDL +y is +z.
    double acc[3], gyro[3];
    input_motion_axes((const float[3]){0, STD_GRAVITY, 0}, (const float[3]){0, 1, 0}, acc, gyro);
    int flat = fabs(acc[0]) < 1e-6 && fabs(acc[1]) < 1e-6 && fabs(acc[2] + 1) < 1e-6 && gyro[2] == 1 && !gyro[0] && !gyro[1];
    input_motion_axes((const float[3]){0, 0, -STD_GRAVITY}, (const float[3]){0, 0, 1}, acc, gyro);
    int upright = fabs(acc[1] + 1) < 1e-6 && gyro[1] == -1;
    CHECK(flat && upright, "SDL -> Switch axes (flat %d, upright %d)", flat, upright);

    // Fusion: flat, then 90 degrees a second about z for 1 s: a quarter turn, x axis now along y
    MotionFusion *m = motion_new();
    uint64_t t = 1000000000ull;
    const double down[3] = {0, 0, -1}, still[3] = {0, 0, 0}, turn[3] = {0, 0, M_PI / 2};
    motion_feed(m, down, still, t);
    for (int i = 0; i < 250; i++) motion_feed(m, down, turn, t += 4000000);
    MotionState ms[16];
    int n = motion_read(m, ms, 16, t + 1000000);
    CHECK(n == 16 && ms[0].sampling == 251 && ms[15].sampling == 236 && ms[0].attributes == 1 &&
          fabs(ms[0].angle[2] - 0.25) < 0.002 && fabs(ms[0].gyro[2] - 0.25) < 1e-6 && near3(ms[0].dir, 0, 1, 0, 0.01) &&
          near3(ms[0].dir + 3, -1, 0, 0, 0.01) && near3(ms[0].dir + 6, 0, 0, 1, 0.01),
          "fusion quarter turn: %d states, sampling %lld, angle %.4f rev, x axis (%.3f %.3f %.3f)", n,
          (long long)ms[0].sampling, (double)ms[0].angle[2], (double)ms[0].dir[0], (double)ms[0].dir[1], (double)ms[0].dir[2]);
    double quarter = ms[0].angle[2];
    CHECK(motion_read(m, ms, 16, t + 300000000ull) == 0, "fusion states 300 ms after the last sample");
    // StartSixAxisSensor again in the middle of the turn (the bow drawn again): the states from before it are gone, and
    // the first one after it starts a fresh angle in a fresh frame; sampling numbers go on.
    for (int i = 0; i < 250; i++) motion_feed(m, down, turn, t += 4000000);
    motion_restart(m);
    int same = motion_read(m, ms, 16, t + 1000000);
    motion_feed(m, down, turn, t += 4000000);
    int next = motion_read(m, ms, 16, t + 1000000);
    CHECK(same == 0 && next == 1 && ms[0].sampling == 502 && fabs(ms[0].angle[2]) < 0.002 &&
          near3(ms[0].dir, 1, 0, 0, 0.01),
          "fusion restart: %d states in the frame of the Start, %d after one sample (sampling %lld, angle %.4f rev)", same,
          next, (long long)ms[0].sampling, (double)ms[0].angle[2]);
    printf("hid feedback: vibration handles %#x %#x, six-axis %#x, at rest when stopped; fusion quarter turn %.4f rev\n",
           vh[0], vh[1], sh[0], quarter);
}

static void probe_input(void) {
    uint32_t pid = ov_handheld() ? 0x20 : 0, other = ov_handheld() ? 0 : 0x20, ids[2] = {0, 0x20};
    uint32_t style = ov_handheld() ? 2 : 1;
    CALL("hid.InitializeNpad", 0);
    CALL("hid.SetSupportedNpadIdType", U(ids), 2);
    CALL("hid.SetSupportedNpadStyleSet", 3);
    CHECK((uint32_t)CALL("hid.GetNpadStyleSet", U(&pid)) == style && (uint32_t)CALL("hid.GetNpadStyleSet", U(&other)) == 0,
          "style sets");
    uint8_t st[5][40];
    memset(st, 0x55, sizeof st);
    static const char *const kinds[] = {"hid.GetNpadStates.FullKey", "hid.GetNpadStates.Handheld",
                                        "hid.GetNpadStates.JoyDual", "hid.GetNpadStates.JoyLeft",
                                        "hid.GetNpadStates.JoyRight"};
    int connected = 0, all_one = 1, no_buttons = 1;
    for (int i = 0; i < 5; i++) {
        all_one &= (int32_t)CALL(kinds[i], U(st[i]), 1, U(&pid)) == 1;
        uint32_t attr;
        uint64_t buttons;
        memcpy(&attr, st[i] + 32, 4);
        memcpy(&buttons, st[i] + 8, 8);
        connected += attr & 1;
        no_buttons &= !buttons;
        if (attr & 1) CHECK(i == (ov_handheld() ? 1 : 0), "%s reports a connected controller", kinds[i]);
    }
    CHECK(all_one && connected == 1 && no_buttons, "controller states: %d connected", connected);
    CHECK((int32_t)CALL("hid.GetNpadStates.FullKey", U(st[0]), 0, U(&pid)) == 0, "zero states requested");
    CALL("hid.SetNpadJoyHoldType", 1);
    uint8_t res[8] = {0}, arg[0x80] = {0};
    CHECK(CALL("hid.ShowControllerSupport", U(res), U(arg)) == 0 && res[0] == 1, "controller support applet");
    CALL("hid.InitializeMouse", 0);
    CALL("hid.InitializeKeyboard", 0);
    uint8_t mouse[40], kb[48];
    CHECK((int32_t)CALL("hid.GetMouseStates", U(mouse), 1) == 1, "mouse states");
    CALL("hid.GetKeyboardState", U(kb));
    probe_feedback(pid, style);
    CALL("hid.DisconnectNpad", U(&pid));
    CHECK((uint32_t)CALL("hid.GetNpadStyleSet", U(&pid)) == 0, "a disconnected controller keeps its style set");
    printf("hid: one connected %s controller (id %#x), no buttons, no mouse/keyboard input\n",
           ov_handheld() ? "handheld" : "full-key", pid);
}

static void probe_media_gfx(void) {
    uint8_t *dec = guest_alloc(0x40), *work = guest_alloc(0x9000);
    uint64_t ws = CALL("codec.GetWorkBufferSize", 48000, 2, 0);
    CHECK(ws > 0 && CALL("codec.GetWorkBufferSize", 44100, 2, 0) == 0, "Opus work buffer size");
    CHECK(CALL("codec.Initialize", U(dec), 48000, 2, U(work), ws, 0) == 0, "InitializeHardwareOpusDecoder");
    CHECK(CALL("codec.Initialize", U(dec), 48000, 3, U(work), ws, 0) != 0, "3 channels accepted");
    CHECK((int32_t)CALL("codec.GetSampleRate", U(dec)) == 48000 && (int32_t)CALL("codec.GetChannelCount", U(dec)) == 2,
          "decoder rate/channels");
    CHECK(CALL("codec.Reset", U(dec)) == 0, "ResetHardwareOpusDecoder");
    CALL("codec.Finalize", U(dec));

    CALL("nv.SetGraphicsAllocator", 0x7000001000, 0x7000002000, 0x7000003000, 0x1234);
    CALL("nv.SetGraphicsDevtoolsAllocator", 0x7000004000, 0x7000005000, 0x7000006000, 0x5678);
    uint8_t *mem = guest_alloc(0x1000);
    CALL("nv.InitializeGraphics", U(mem), 0x1000);
    const OvNvState *s = ov_nv_state();
    CHECK(s->graphics.alloc == 0x7000001000 && s->graphics.realloc == 0x7000003000 && s->graphics.user == 0x1234 &&
          s->devtools.free == 0x7000005000 && s->devtools.user == 0x5678 && s->memory == U(mem) &&
          s->memory_size == 0x1000 && s->initialized, "nv state not recorded");
    void *display = NULL, *layer = NULL, *window = NULL;
    CHECK(CALL("vi.OpenDefaultDisplay", U(&display)) != 0, "OpenDefaultDisplay before vi::Initialize");
    CALL("vi.Initialize", 0);
    CHECK(CALL("vi.OpenDefaultDisplay", U(&display)) == 0 && display, "OpenDefaultDisplay");
    CHECK(CALL("vi.CreateLayer", U(&layer), U(display)) == 0 && layer, "CreateLayer");
    CHECK(CALL("vi.CreateLayer", U(&layer), U(&layer)) != 0, "CreateLayer on an unknown display");
    CHECK(CALL("vi.GetNativeWindow", U(&window), U(layer)) == 0 && window, "GetNativeWindow");
    uint64_t ev = U(guest_alloc(0x100));
    CHECK(CALL("vi.GetDisplayVsyncEvent", ev, U(display)) == 0, "GetDisplayVsyncEvent");
    int vsync = event_works(ev, 1);
    CHECK(vsync, "the vsync event does not work as an auto-clear system event");
    printf("codec/nv/vi: Opus decoder state kept, allocators and work memory recorded, display %p layer %p window %p, "
           "vsync event %s\n", display, layer, window, vsync ? "bound (auto-clear)" : "FAILED");
}

// ---- software keyboard (swkbd.c): ShowKeyboardArg built by the SDK's own setters, as the horse naming does
enum { KB_TYPE = 0x00, KB_DISABLED = 0x1c, KB_HEADER = 0x24, KB_GUIDE = 0x1a8, KB_MAX_LEN = 0x3ac, KB_MIN_LEN = 0x3b0,
       KB_UTF8 = 0x3bd, KB_INIT_LEN = 0x3c4, STRING_SIZE = 0x7d4, SWKBD_CANCELED = 0x29f };
#define ARG_WORK (RT_V143 ? 0x518 : 0x4d0)   // SDK 20.5 grew KeyboardConfig (swkbd.c)
#define ARG_WORK_SIZE (ARG_WORK + 8)
#define ARG_SIZE (RT_V143 ? 0x548 : 0x500)

static uint64_t sdk(const char *name) {
    uint64_t a = ldr_symbol(name);
    if (!a) rt_fatal("probe-services: the SDK exports no %s", name);
    return a;
}

// NUL-terminated UTF-16 copy of utf8 in guest memory
static uint16_t *u16(const char *utf8) {
    uint16_t *g = guest_alloc(0x200);
    utf8_to_utf16(utf8, g, 128);
    return g;
}

static int u16_is(const uint16_t *a, const char *utf8) {
    uint16_t b[128];
    utf8_to_utf16(utf8, b, 128);
    for (size_t i = 0;; i++) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
}

typedef struct {
    int preset;
    uint32_t type, max, min, disabled;
    int utf8, window;
    const char *initial, *answer, *want;
    uint32_t result;
} KbCase;

static void probe_swkbd(void) {
    uint64_t make = sdk("_ZN2nn5swkbd10MakePresetEPNS0_14KeyboardConfigENS0_6PresetE");
    uint64_t header = sdk("_ZN2nn5swkbd13SetHeaderTextEPNS0_14KeyboardConfigEPKDs");
    uint64_t guide = sdk("_ZN2nn5swkbd12SetGuideTextEPNS0_14KeyboardConfigEPKDs");
    uint64_t initial = sdk("_ZN2nn5swkbd14SetInitialTextEPNS0_15ShowKeyboardArgEPKDs");
    uint64_t string_size = sdk("_ZN2nn5swkbd27GetRequiredStringBufferSizeEv");
    uint64_t work_size = sdk("_ZN2nn5swkbd25GetRequiredWorkBufferSizeEb");
    CHECK(rt_call(gc, string_size, 0) == STRING_SIZE && rt_call(gc, work_size, 1, 0) == 0x1000, "SDK swkbd buffer sizes");
    uint8_t *arg = guest_alloc(ARG_SIZE), *work = guest_alloc(0x1000);
    uint64_t *str = guest_alloc(16);
    uint16_t *out = guest_alloc(STRING_SIZE);
    str[0] = U(out), str[1] = STRING_SIZE;
    uint64_t w = U(work), ws = 0x1000;

    // the SDK setters write where swkbd.c reads
    rt_call(gc, make, 2, U(arg), 2);
    rt_call(gc, header, 2, U(arg), U(u16("Hdr")));
    rt_call(gc, guide, 2, U(arg), U(u16("Gd")));
    memcpy(arg + ARG_WORK, &w, 8), memcpy(arg + ARG_WORK_SIZE, &ws, 8);
    rt_call(gc, initial, 2, U(arg), U(u16("Zelda")));
    int32_t init_len;
    memcpy(&init_len, arg + KB_INIT_LEN, 4);
    CHECK(u16_is((uint16_t *)(arg + KB_HEADER), "Hdr") && u16_is((uint16_t *)(arg + KB_GUIDE), "Gd") && init_len == 5,
          "ShowKeyboardArg layout: header, guide or initial text not where swkbd.c reads them");

    const char *def = ui_text(UI_SWKBD_DEFAULT_NAME);
    const KbCase cases[] = {
        {2, 8, 9, 0, 0x2c, 0, 0, NULL, "Epona", "Epona", 0},                  // TotK's horse naming (main+0x1ecbf64)
        {2, 8, 9, 0, 0x2c, 0, 0, NULL, "ABCDEFGHIJKL", "ABCDEFGHI", 0},        // at most 9
        {2, 8, 9, 0, 0x2c, 0, 0, NULL, "a@b%c\\d e/f", "abcd e/f", 0},         // '@' '%' '\' disabled
        {2, 8, 9, 0, 0x2c, 0, 0, NULL, "<cancel>", "", SWKBD_CANCELED},
        {2, 8, 9, 0, 0x2c, 0, 0, NULL, "<initial>", def, 0},                   // no initial text: the default name
        {0, 2, 0, 0, 0, 0, 0, "Zelda", "<initial>", "Zelda", 0},                // initial text from the work buffer
        {0, 2, 0, 0, 0, 1, 0, NULL, "\xe5\xb0\x8f\xe9\xa9\xac" "Ab", "\xe5\xb0\x8f\xe9\xa9\xac" "Ab", 0},   // UTF-8 out
        {0, 2, 3, 0, 0, 0, 0, NULL, "\xe5\xb0\x8f\xe9\xa9\xac\xf0\x9f\x90\xb4", "\xe5\xb0\x8f\xe9\xa9\xac", 0},   // no half pair
        {0, 1, 0, 0, 0, 0, 0, NULL, "a1b2c3", "123", 0},                       // number pad
        {0, 2, 9, 3, 0, 0, 0, NULL, "ab", "", SWKBD_CANCELED},                  // shorter than the minimum
        {2, 8, 9, 0, 0x2c, 0, 1, NULL, "ok", def, 0},                          // the window, OK pressed
        {2, 8, 9, 0, 0x2c, 0, 1, NULL, "cancel", "", SWKBD_CANCELED},          // the window, Cancel pressed
    };
    int bad = 0, n = (int)(sizeof cases / sizeof *cases);
    for (int i = 0; i < n; i++) {
        const KbCase *k = &cases[i];
        memset(arg, 0, ARG_SIZE), memset(work, 0, 0x1000), memset(out, 0x55, STRING_SIZE);
        rt_call(gc, make, 2, U(arg), k->preset);
        memcpy(arg + KB_TYPE, &k->type, 4), memcpy(arg + KB_MAX_LEN, &k->max, 4), memcpy(arg + KB_MIN_LEN, &k->min, 4);
        if (k->disabled) memcpy(arg + KB_DISABLED, &k->disabled, 4);
        arg[KB_UTF8] = (uint8_t)k->utf8;
        memcpy(arg + ARG_WORK, &w, 8), memcpy(arg + ARG_WORK_SIZE, &ws, 8);
        if (k->initial) rt_call(gc, initial, 2, U(arg), U(u16(k->initial)));
        set_env(k->window ? "TOTK_SWKBD_TEST" : "TOTK_SWKBD_ANSWER", k->answer);
        uint32_t r = (uint32_t)CALL("swkbd.ShowKeyboard", U(str), U(arg));
        set_env("TOTK_SWKBD_TEST", NULL), set_env("TOTK_SWKBD_ANSWER", NULL);
        int ok = r == k->result && (k->utf8 ? !strcmp((const char *)out, k->want) : u16_is(out, k->want));
        checks++;
        if (!ok) bad++, printf("  FAIL: swkbd case %d (%s%s): result %#x\n", i, k->window ? "window " : "", k->answer, r);
    }
    failures += bad;
    // after the window the game reads nothing until everything is released; with nothing held one read lifts that
    uint64_t b;
    int32_t lx, ly, rx, ry;
    input_poll_ui(&b, &lx, &ly, &rx, &ry);
    if (!b) input_poll(&b, &lx, &ly, &rx, &ry);
    printf("swkbd: %d of %d cases (arguments from the SDK's MakePreset/SetHeaderText/SetGuideText/SetInitialText; "
           "horse-name limits, disabled keys, cancel 0x29f, default name \"%s\", UTF-8 out, surrogate pairs, number pad, "
           "minimum length, the Win32 window with OK and Cancel)\n", n - bad, n, def);
}

// ---- the menu's input gate: Minus+Plus is consumed, a menu that is open leaves the game a neutral pad
static void probe_menu_gate(void) {
    const uint64_t minus = 1ull << 11, plus = 1ull << 10, a = 1ull;
    int held = 0;
    int32_t st[4] = {100, -200, 300, -400};
    uint64_t b = minus | a;   // Minus alone, a frame before Plus: reaches the guest unchanged
    int t1 = input_menu_gate(&held, 0, &b, st);
    CHECK(!t1 && b == (minus | a) && st[0] == 100 && st[3] == -400 && !held, "Minus alone changed: toggle %d buttons %#llx", t1,
          (unsigned long long)b);
    b = minus | plus | a;     // both down: the guest sees neither, the menu toggles once
    int t2 = input_menu_gate(&held, 0, &b, st);
    CHECK(t2 == 1 && b == a && held, "Minus+Plus: toggle %d buttons %#llx", t2, (unsigned long long)b);
    b = minus | plus;         // still held with the menu open: no second toggle, a neutral pad
    int t3 = input_menu_gate(&held, 1, &b, st);
    CHECK(!t3 && b == 0 && st[0] == 0 && st[1] == 0 && st[2] == 0 && st[3] == 0, "menu open: toggle %d buttons %#llx", t3,
          (unsigned long long)b);
    b = a | plus; st[0] = 5;  // the menu is open and the pad is neutral whatever is pressed
    input_menu_gate(&held, 1, &b, st);
    CHECK(b == 0 && st[0] == 0, "an open menu left buttons %#llx", (unsigned long long)b);
    b = 0;                    // released, then pressed again toggles again
    input_menu_gate(&held, 0, &b, st);
    b = minus | plus;
    CHECK(input_menu_gate(&held, 0, &b, st) == 1, "the second hold did not toggle");
    // Plus released first while Minus is still down: Minus must not leak; a new Minus press after both are up passes
    held = 0;
    b = minus | plus;
    input_menu_gate(&held, 0, &b, st);
    b = minus;
    int t4 = input_menu_gate(&held, 0, &b, st);
    CHECK(!t4 && b == 0 && held, "Minus left down after Plus leaked: buttons %#llx", (unsigned long long)b);
    b = 0;
    input_menu_gate(&held, 0, &b, st);
    b = minus | a;
    int t5 = input_menu_gate(&held, 0, &b, st);
    CHECK(!t5 && b == (minus | a) && !held, "Minus after the release did not pass: buttons %#llx", (unsigned long long)b);
    printf("menu gate: Minus alone passes, Minus+Plus is consumed and toggles once per hold, an open menu gives a neutral pad\n");
}

// ---- shared fonts: the extracted files when present (the missing case is a fatal child)
static void probe_fonts(void) {
    char dir[PLAT_PATH_MAX], path[PLAT_PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/0-standard.bfttf", ov_shared_font_dir(dir, sizeof dir));
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("pl: no shared fonts in %s (tools/extract_fonts.py); Address/Size checked as named fatals only\n", dir);
        return;
    }
    uint8_t head[12];
    size_t got = fread(head, 1, sizeof head, f);
    plat_fseek64(f, 0, SEEK_END);
    int64_t len = plat_ftell64(f);
    fclose(f);
    uint64_t size = CALL("pl.GetSharedFontSize", 0), addr = CALL("pl.GetSharedFontAddress", 0);
    CHECK(got == sizeof head && size && size + 8 == (uint64_t)len, "shared font 0: size %llu, file %lld bytes",
          (unsigned long long)size, (long long)len);
    const uint8_t *p = (const uint8_t *)(uintptr_t)addr;
    CHECK(addr && !memcmp(p - 8, head, sizeof head) && ov_bfttf_size(p - 8, size + 8) == size,
          "shared font 0: the address is not 8 bytes into the BFTTF file");
    CHECK(CALL("pl.GetSharedFontAddress", 0) == addr, "shared font 0: the address changed");
    printf("pl: shared font 0 (Standard) at %#llx, %llu bytes, BFTTF header checked (%s)\n", (unsigned long long)addr,
           (unsigned long long)size, path);
}

// The host catalog (ui_text.c): every menu id has its own English and Russian text, the software keyboard keeps its
// game-language rows, and menu_text follows TOTK_MENU_LANG.
static void probe_menu_text(void) {
    static const char *const langs[] = {"en", "ja", "fr", "de", "it", "es", "nl", "pt", "ru", "zh-Hans", "zh-Hant", "ko"};
    for (int id = UI_MENU_FIRST; id <= UI_MENU_LAST_ID; id++) {
        CHECK(ui_text_defined("en", (UiTextId)id) && ui_text_defined("ru", (UiTextId)id), "menu text id %d lacks en or ru", id);
        CHECK(*ui_text_in("en", (UiTextId)id) && *ui_text_in("ru", (UiTextId)id), "menu text id %d is empty", id);
        // the description pane keeps the whole text and a note after it (option_rows.cpp desc_set)
        CHECK(strlen(ui_text_in("en", (UiTextId)id)) + 256 < UI_DESC_BYTES &&
              strlen(ui_text_in("ru", (UiTextId)id)) + 256 < UI_DESC_BYTES, "menu text id %d is too long for the "
              "description pane", id);
    }
    CHECK(!ui_text_defined("en", UI_TEXT_COUNT) && !*ui_text_in("ru", UI_TEXT_COUNT), "an id past the catalog gives text");
    // other languages fall back to English for the menu, and keep their own keyboard texts
    CHECK(!strcmp(ui_text_in("ja", UI_MENU_CONTINUE), ui_text_in("en", UI_MENU_CONTINUE)), "ja menu text is not English");
    CHECK(!strcmp(ui_text_in("ru", UI_SWKBD_OK), "ОК") && !strcmp(ui_text_in("de", UI_SWKBD_CANCEL), "Abbrechen") &&
          !strcmp(ui_text_in("en", UI_SWKBD_OK), "OK"), "keyboard texts changed");
    // language names: the same string in every row
    for (int id = UI_MENU_LANGNAME_EN; id <= UI_MENU_LANGNAME_KO; id++)
        for (size_t i = 1; i < sizeof langs / sizeof *langs; i++)
            CHECK(!strcmp(ui_text_in(langs[i], (UiTextId)id), ui_text_in("en", (UiTextId)id)), "language name %d differs in %s", id, langs[i]);
    CHECK(!strcmp(ui_text_in("en", UI_MENU_LANGNAME_RU), "Русский"), "Russian is not shown as Russian");
    // the overlay formats keep their conversions
    CHECK(strstr(ui_text_in("ru", UI_MENU_OV_GAME_PCT), "%.0f%%") && strstr(ui_text_in("ru", UI_MENU_OV_MAX_MS), "%.1f"),
          "overlay format lost its conversion");
    // menu_text follows the setting live
    cfg_set("TOTK_MENU_LANG", "ru");
    CHECK(menu_text(UI_MENU_CONTINUE) == ui_text_in("ru", UI_MENU_CONTINUE), "menu_text ignores MENU_LANG=ru");
    cfg_set("TOTK_MENU_LANG", "en");
    CHECK(menu_text(UI_MENU_CONTINUE) == ui_text_in("en", UI_MENU_CONTINUE), "menu_text did not follow MENU_LANG=en");
    cfg_set("TOTK_MENU_LANG", "");
    const char *sys = plat_ui_language();
    int ru = !strncmp(sys, "ru", 2);
    CHECK(menu_text(UI_MENU_CONTINUE) == ui_text_in(ru ? "ru" : "en", UI_MENU_CONTINUE), "menu_text auto ignores the system language");
}

static void probe_main(Cpu *c) {
    gc = c;
    probe_oe();
    probe_messages();
    probe_applet();
    probe_account();
    probe_time();
    CHECK((uint32_t)CALL("pl.GetSharedFontLoadState", 0) == 1, "shared font load state");
    probe_input();
    probe_media_gfx();
    probe_swkbd();
    probe_menu_text();
    probe_menu_gate();
    probe_fonts();
}

// ---- child processes
// Runs this executable again with the same command line and TOTK_PROBE_SERVICES_CHILD=<mode>; its stdout and stderr
// into out. Returns the exit code.
static uint32_t run_child(const char *mode, const char *env, char *out, size_t cap) {
    char name[64] = "";
    const char *eq = env ? strchr(env, '=') : NULL;
    if (eq) snprintf(name, sizeof name, "%.*s", (int)(eq - env), env), plat_setenv(name, eq + 1);
    int code = plat_run_self(CHILD_ENV, mode, out, cap, 0);
    if (eq) plat_setenv(name, NULL);
    if (code < 0) rt_fatal("probe-services: cannot start the %s child", mode);
    return (uint32_t)code;
}

static const char *child_mode;

static void child_main(Cpu *c) {
    gc = c;
    if (!strcmp(child_mode, "uid")) {
        uint64_t h[3] = {0}, uid[2] = {0};
        CALL("account.Initialize", 0);
        uint8_t opened = (uint8_t)CALL("account.TryOpenPreselectedUser", U(h));
        uint32_t r = (uint32_t)CALL("account.GetUserId", U(uid), U(h));
        printf("child uid %016llx%016llx (open %u, result %#x)\n", (unsigned long long)uid[1], (unsigned long long)uid[0],
               opened, r);
        return;
    }
    uint8_t *buf = guest_alloc(0x1000);
    if (!strcmp(child_mode, "nvn")) {
        uint64_t known = CALL("nvnBootstrapLoader", U("nvnDeviceGetProcAddress"));
        uint64_t unknown = CALL("nvnBootstrapLoader", U("nvnNoSuchProc"));
        printf("child nvn: known %#llx expected %#llx unknown %#llx\n", (unsigned long long)known,
               (unsigned long long)nvn_proc_address("nvnDeviceGetProcAddress"), (unsigned long long)unknown);
        return;
    }
    if (!strcmp(child_mode, "opus")) CALL("codec.Decode", U(buf), U(buf + 8), U(buf + 16), U(buf + 64), 0x100, U(buf + 0x800), 16);
    else if (!strcmp(child_mode, "font-address")) CALL("pl.GetSharedFontAddress", 0);
    else if (!strcmp(child_mode, "font-size")) CALL("pl.GetSharedFontSize", 0);
    else rt_fatal("probe-services: unknown child case %s", child_mode);
    printf("child %s: the call returned\n", child_mode);
}

static int run_as_child(const char *mode) {
    child_mode = mode;
    bind_addresses();
    hostfn_seal();
    kern_run_on_guest_main(child_main);
    return 0;
}

static void probe_children(void) {
    static char out[16384];
    uint32_t code = run_child("uid", NULL, out, sizeof out);
    unsigned long long hi = 0, lo = 0;
    const char *p = strstr(out, "child uid ");
    int parsed = p && sscanf(p + 10, "%16llx%16llx", &hi, &lo) == 2;
    int same = parsed && code == 0 && hi == probe_uid[1] && lo == probe_uid[0];
    CHECK(same, "the second run's account UID differs (exit %u): %s", code, out);
    printf("account: UID %016llx%016llx in %s/account.uid, the same in a second run: %s\n",
           (unsigned long long)probe_uid[1], (unsigned long long)probe_uid[0], rt_config()->profile, same ? "yes" : "NO");

    // nvnBootstrapLoader returns the synthetic address of a proc (nvn_procs.c), 0 for an unknown name
    code = run_child("nvn", NULL, out, sizeof out);
    unsigned long long known = 0, expected = 0, unknown = 1;
    p = strstr(out, "child nvn: ");
    parsed = p && sscanf(p, "child nvn: known %llx expected %llx unknown %llx", &known, &expected, &unknown) == 3;
    int nvn_ok = parsed && code == 0 && known && known == expected && unknown == 0;
    CHECK(nvn_ok, "child nvn: exit %u, output: %s", code, out);
    if (nvn_ok) ov("nvnBootstrapLoader")->calls++;
    printf("nvnBootstrapLoader: nvnDeviceGetProcAddress -> %#llx (proc table %#llx), unknown name -> %#llx\n", known,
           expected, unknown);

    for (int i = 0; i < NFATAL; i++) {
        code = run_child(fatals[i].child, fatals[i].env, out, sizeof out);
        int ok = code == 70 && strstr(out, fatals[i].fatal);
        CHECK(ok, "child %s: exit %u, output: %s", fatals[i].child, code, out);
        if (ok) ov(fatals[i].key)->calls++;
        const char *line = strstr(out, "FATAL: ");
        int len = line ? (int)strcspn(line, "\r\n") : 0;
        printf("fatal stub %-20s child exit %u: %.*s\n", fatals[i].key, code, len, line ? line : "(no FATAL line)");
    }
}

int probe_services(void) {
    const char *child = getenv(CHILD_ENV);
    if (child && *child) return run_as_child(child);
    ldr_load(rt_config()->exefs);

    // Names bound by the loader: each override and observer probed here must match a relocation.
    int bound = 0, obs_bound = 0;
    for (int i = 0; i < 4096; i++) {
        const char *n = hostfn_name(RT_HOSTFN_BASE + 16ull * (uint64_t)i);
        if (!n) break;
        for (int k = 0; k < NOV; k++)
            if (!strcmp(ovs[k].name, n) && !ovs[k].bound) ovs[k].bound = 1, bound++;
        for (int k = 0; k < NOBS; k++)
            if (!strcmp(observers[k], n)) obs_bound++;
    }
    for (int k = 0; k < NOV; k++)   // 1.2.1 lists no add-on content
        CHECK(ovs[k].bound || (!RT_V143 && !strcmp(ovs[k].key, "aoc.ListAddOnContent")), "override %s matches no relocation",
              ovs[k].name);
    for (int k = 0; k < NOBS; k++) CHECK(hostfn_observer(observers[k]), "observer %s is not linked", observers[k]);
    CHECK(obs_bound == NOBS, "%d of %d observers match a JUMP_SLOT", obs_bound, NOBS);

    // Every linked override is either probed here or belongs to another probe or owner (nn::fs: fs probe, fibers: kernel probe,
    // movie::Decoder: --probe-movie, memcpy: the graphics write-back in nvn_api_core.c).
    int linked = hostfn_table_size(0), others = 0;
    for (int i = 0; i < linked; i++) {
        const char *n = hostfn_table_name(0, i);
        int mine = 0;
        for (int k = 0; k < NOV; k++) mine |= !strcmp(ovs[k].name, n);
        int theirs = !strncmp(n, "_ZN2nn2fs", 9) || !strncmp(n, "_ZN5movie", 9) || !strcmp(n, "memcpy") ||
                     !strcmp(n, KERN_SYM_INITIALIZE_FIBER) ||
                     !strcmp(n, KERN_SYM_FINALIZE_FIBER) || !strcmp(n, KERN_SYM_SWITCH_TO_FIBER) ||
                     !strcmp(n, KERN_SYM_GET_CURRENT_FIBER);
        others += theirs;
        CHECK(mine || theirs, "override %s is linked but not probed", n);
    }
    printf("bound: %d of %d overrides and %d of %d observers match relocations of the loaded modules\n", bound, NOV,
           obs_bound, NOBS);

    bind_addresses();
    hostfn_seal();
    kern_run_on_guest_main(probe_main);
    probe_children();

    int called = 0;
    for (int k = 0; k < NOV; k++) {
        called += ovs[k].calls > 0;
        CHECK(ovs[k].calls > 0, "override %s was not called", ovs[k].key);
    }
    printf("called: %d of %d overrides through rt_dispatch (%d named fatal stubs in child processes); %d other "
           "overrides belong to the kernel and fs probes\n", called, NOV, NFATAL, others);
    printf("overrides %d, observers %d\n", linked, hostfn_table_size(1));
    if (failures) {
        printf("probe-services: FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("probe-services: passed\n");
    return 0;
}
