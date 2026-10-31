#!/usr/bin/env python3
"""Fails when runtime code outside runtime/platform/ uses the Win32 API directly.

Runtime code goes through runtime/platform/platform.h. The Win32 back ends of the window system are Win32 by design and
are not scanned: files named *_win.c / *_win.cpp (runtime/ and runtime/ui/), whose SDL counterparts (*_sdl.*) serve
other hosts. Any other file that still uses Win32 goes into ALLOWED with the reason; a file in ALLOWED that no longer
matches is noted, so the entry gets removed.

Usage: python tools/check_platform.py [--root DIR]   (DIR holds runtime/; default: the repository root)
"""
import argparse
import re
import sys
from pathlib import Path

# Per-file allowlist of shared files that still use Win32, with the reason. Empty: every shared file is portable.
ALLOWED = {}

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"](windows|winsock2|ws2tcpip|synchapi|memoryapi|processthreadsapi|'
                     r'fileapi|handleapi|libloaderapi|winbase|winnt)\.h[>"]', re.M)
CALLS = [
    "CreateFile[AW]?", "ReadFile", "WriteFile", "CloseHandle", "DeleteFile[AW]?", "MoveFileEx[AW]?", "CreateDirectory[AW]?",
    "FindFirstFile[AW]?", "GetFileAttributes[AW]?", "GetFileSizeEx", "SetFilePointerEx", "SetEndOfFile",
    "CreatePipe", "CreateProcess[AW]?", "TerminateProcess", "GetModuleFileName[AW]?", "GetEnvironmentVariable[AW]?",
    "VirtualAlloc(Ex)?", "VirtualFree", "VirtualProtect", "VirtualQuery", "MapViewOfFile[A-Za-z0-9]*", "CreateFileMapping[AW]?",
    "GetWriteWatch", "ResetWriteWatch", "AddVectoredExceptionHandler", "RemoveVectoredExceptionHandler",
    "CreateThread", "_beginthread(ex)?", "GetCurrentThread(Id)?", "SetThreadAffinityMask", "SetThreadDescription",
    "SetThreadPriority", "WaitForSingleObject(Ex)?", "WaitForMultipleObjects", "CreateEvent[AW]?", "CreateSemaphore[AW]?",
    "CreateMutex[AW]?", "CreateWaitableTimer(Ex)?[AW]?", "SetWaitableTimer(Ex)?", "Sleep(Ex)?", "SwitchToThread",
    "QueryPerformanceCounter", "QueryPerformanceFrequency", "GetTickCount(64)?", "GetSystemTimePreciseAsFileTime",
    "GetSystemInfo", "GetLogicalProcessorInformation(Ex)?", "SRWLOCK", "CONDITION_VARIABLE", "CRITICAL_SECTION",
    "InitializeCriticalSection[A-Za-z]*", "EnterCriticalSection", "LeaveCriticalSection",
    "(Acquire|Release)SRWLock(Exclusive|Shared)", "WakeAllConditionVariable", "WakeConditionVariable",
    "SleepConditionVariableSRW", "InitOnceExecuteOnce", "INIT_ONCE", "Interlocked[A-Za-z0-9]*", "TlsAlloc", "FlsAlloc",
    "LoadLibrary[AW]?", "GetProcAddress", "FreeLibrary", "HANDLE", "DWORD", "LPVOID", "WINAPI",
    "SuspendThread", "ResumeThread", "GetThreadContext", "SetThreadContext", "OpenThread", "CreateFiber(Ex)?",
    "ConvertThreadToFiber(Ex)?", "ConvertFiberToThread", "SwitchToFiber", "DeleteFiber", "BCrypt[A-Za-z]*",
    "_aligned_malloc", "_aligned_free", "_stricmp", "_strnicmp", "_strdup", "_wfopen", "_fseeki64", "_ftelli64",
    "localtime_s", "strtok_s", "GetModuleHandle(Ex)?[AW]?", "ExitProcess", "SetEnvironmentVariable[AW]?", "_putenv(_s)?",
    "GetLastError", "MultiByteToWideChar", "WideCharToMultiByte", "HWND", "LARGE_INTEGER", "FILETIME",
    "MAX_PATH", "GetActiveProcessorCount", "timeBeginPeriod",
]
CALL_RE = re.compile(r"\b(" + "|".join(CALLS) + r")\b")


def strip(text):
    """Blank comments and string literals, keeping line numbers."""
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    text = re.sub(r"/\*.*?\*/", blank, text, flags=re.S)
    text = re.sub(r"//[^\n]*", blank, text)
    return re.sub(r'"(?:\\.|[^"\\\n])*"', blank, text)


def scan(path):
    raw = path.read_text(encoding="utf-8", errors="replace")
    hits = [("include", m.group(0).strip()) for m in INCLUDE.finditer(raw)]
    code = strip(raw)
    hits += [("call", m.group(1)) for m in CALL_RE.finditer(code)]
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    ap.add_argument("--runtime", default=None, help="runtime dir (default ROOT/runtime)")
    a = ap.parse_args()
    rt = Path(a.runtime) if a.runtime else Path(a.root) / "runtime"
    bad, remaining = [], {}
    files = [p for pat in ("*.c", "*.h", "*.cpp", "ui/*.cpp", "ui/*.h") for p in rt.glob(pat)]
    for p in sorted(files):
        if p.stem.endswith("_win"):
            continue
        hits = scan(p)
        if p.name in ALLOWED:
            remaining[p.name] = len(hits)
        elif hits:
            bad.append((p.name, hits))
    for name, hits in bad:
        kinds = sorted({h[1] for h in hits})
        print(f"FAIL runtime/{name}: {len(hits)} Win32 uses outside runtime/platform/ ({', '.join(kinds[:8])})")
    stale = [n for n, c in remaining.items() if c == 0]
    for n in stale:
        print(f"note runtime/{n}: allowlisted but no Win32 use left; remove it from ALLOWED")
    print("remaining Win32 uses per allowlisted file:")
    for n, c in sorted(remaining.items(), key=lambda kv: (-kv[1], kv[0])):
        print(f"  {c:5d}  runtime/{n}  ({ALLOWED[n]})")
    print(f"total {sum(remaining.values())} in {sum(1 for c in remaining.values() if c)} files")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
