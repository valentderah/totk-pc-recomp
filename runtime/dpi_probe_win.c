// --probe-dpi: the process DPI awareness that totk.manifest declares, and what the primary monitor reports under it.
// A DPI-unaware process gets every window and monitor size scaled down by the display scale and its output stretched
// back up by Windows; the probe fails unless the process is per-monitor aware (v2). Needs no game files and no GPU.
#include <stdio.h>
#include <windows.h>

#include "host.h"

static const char *awareness_name(DPI_AWARENESS_CONTEXT ctx) {
    if (AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return "per-monitor aware v2";
    switch (GetAwarenessFromDpiAwarenessContext(ctx)) {
    case DPI_AWARENESS_UNAWARE: return "unaware";
    case DPI_AWARENESS_SYSTEM_AWARE: return "system aware";
    case DPI_AWARENESS_PER_MONITOR_AWARE: return "per-monitor aware v1";
    default: return "invalid";
    }
}

int probe_dpi(void) {
    DPI_AWARENESS_CONTEXT ctx = GetThreadDpiAwarenessContext();
    MONITORINFO mi = {sizeof mi};
    HMONITOR mon = MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
    int have = GetMonitorInfoW(mon, &mi);
    printf("dpi: awareness %s\n", awareness_name(ctx));
    printf("dpi: system dpi %u (scale %u%%)\n", GetDpiForSystem(), GetDpiForSystem() * 100 / 96);
    if (have)
        printf("dpi: primary monitor %ldx%ld at %ld,%ld; screen metrics %dx%d\n", mi.rcMonitor.right - mi.rcMonitor.left,
               mi.rcMonitor.bottom - mi.rcMonitor.top, mi.rcMonitor.left, mi.rcMonitor.top, GetSystemMetrics(SM_CXSCREEN),
               GetSystemMetrics(SM_CYSCREEN));
    int ok = AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) && have;
    printf("probe-dpi: %s\n", ok ? "PASS" : "FAIL (the process is not per-monitor aware v2)");
    return ok ? 0 : 1;
}
