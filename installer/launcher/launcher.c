/* TotK-Setup.exe: starts python\pythonw.exe -m installer.gui with this exe's folder as the working directory.

   No console window. The two message strings come from the catalog (installer/text/<lang>.json, ids launcher.title and
   launcher.python_missing); tools/make_release.py writes them into launcher_text.h before it compiles this file.
   Build: llvm/bin/llvm-windres -I runtime -O coff launcher.rc -o launcher_rc.o, then
   llvm/bin/clang -O2 -mwindows launcher.c launcher_rc.o -o TotK-Setup.exe */
#include <windows.h>
#include <wchar.h>

#include "launcher_text.h"

#define LANGID_RUSSIAN 0x19     /* primary language id */
#define LANGID_PT_BR 0x0416     /* the whole LANGID: Portugal's Portuguese stays English */
#define PATH_CHARS (MAX_PATH * 2)

static void fail(void)
{
    LANGID id = GetUserDefaultUILanguage();
    int russian = (id & 0x3ff) == LANGID_RUSSIAN, brazilian = id == LANGID_PT_BR;
    MessageBoxW(NULL,
                russian ? LAUNCHER_PYTHON_MISSING_RU : brazilian ? LAUNCHER_PYTHON_MISSING_PT_BR
                                                                 : LAUNCHER_PYTHON_MISSING_EN,
                russian ? LAUNCHER_TITLE_RU : brazilian ? LAUNCHER_TITLE_PT_BR : LAUNCHER_TITLE_EN,
                MB_OK | MB_ICONERROR);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command_line, int show)
{
    wchar_t folder[PATH_CHARS], python[PATH_CHARS], command[PATH_CHARS + 64];
    DWORD n = GetModuleFileNameW(NULL, folder, PATH_CHARS);
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    (void)instance; (void)previous; (void)command_line; (void)show;
    if (n == 0 || n >= PATH_CHARS) { fail(); return 1; }
    while (n > 0 && folder[n - 1] != L'\\') n--;
    folder[n > 0 ? n - 1 : 0] = 0;
    if (swprintf(python, PATH_CHARS, L"%ls\\python\\pythonw.exe", folder) < 0 ||
        GetFileAttributesW(python) == INVALID_FILE_ATTRIBUTES) { fail(); return 1; }
    if (swprintf(command, PATH_CHARS + 64, L"\"%ls\" -m installer.gui", python) < 0) { fail(); return 1; }
    if (!CreateProcessW(python, command, NULL, NULL, FALSE, 0, NULL, folder, &si, &pi)) { fail(); return 1; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
