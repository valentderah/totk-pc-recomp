/* TotK-Setup.exe: starts python\pythonw.exe -m installer.gui with this exe's folder as the working directory.

   No console window. The two message strings come from the catalog (installer/text/<lang>.json, ids launcher.title and
   launcher.python_missing); tools/make_release.py writes them into launcher_text.h before it compiles this file.
   Build: llvm/bin/llvm-windres -I runtime -O coff launcher.rc -o launcher_rc.o, then
   llvm/bin/clang -O2 -mwindows launcher.c launcher_rc.o -o TotK-Setup.exe */
#include <windows.h>
#include <wchar.h>

#include "launcher_text.h"

#define LANGID_PT_BR 0x0416     /* the whole LANGID: Portugal's Portuguese stays English */
#define PATH_CHARS (MAX_PATH * 2)

/* The language of the window (installer/guilogic.py ui_language_for_langid): Brazilian Portuguese, Simplified Chinese
   (zh-CN, zh-SG) and Traditional Chinese (zh-TW, zh-HK, zh-MO) by the whole LANGID, the rest by the primary language,
   else English. */
static void fail(void)
{
    static const struct { WORD primary; const wchar_t *title, *text; } by_primary[] = {
        { LANG_RUSSIAN, LAUNCHER_TITLE_RU, LAUNCHER_PYTHON_MISSING_RU },
        { LANG_GERMAN, LAUNCHER_TITLE_DE, LAUNCHER_PYTHON_MISSING_DE },
        { LANG_FRENCH, LAUNCHER_TITLE_FR, LAUNCHER_PYTHON_MISSING_FR },
        { LANG_SPANISH, LAUNCHER_TITLE_ES, LAUNCHER_PYTHON_MISSING_ES },
        { LANG_ITALIAN, LAUNCHER_TITLE_IT, LAUNCHER_PYTHON_MISSING_IT },
        { LANG_DUTCH, LAUNCHER_TITLE_NL, LAUNCHER_PYTHON_MISSING_NL },
        { LANG_JAPANESE, LAUNCHER_TITLE_JA, LAUNCHER_PYTHON_MISSING_JA },
        { LANG_KOREAN, LAUNCHER_TITLE_KO, LAUNCHER_PYTHON_MISSING_KO },
    };
    LANGID ui = GetUserDefaultUILanguage();
    const wchar_t *title = LAUNCHER_TITLE_EN, *text = LAUNCHER_PYTHON_MISSING_EN;
    if (ui == LANGID_PT_BR) {
        title = LAUNCHER_TITLE_PT_BR; text = LAUNCHER_PYTHON_MISSING_PT_BR;
    } else if (ui == MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED) ||
               ui == MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SINGAPORE)) {
        title = LAUNCHER_TITLE_ZH_HANS; text = LAUNCHER_PYTHON_MISSING_ZH_HANS;
    } else if (ui == MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL) ||
               ui == MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_HONGKONG) ||
               ui == MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_MACAU)) {
        title = LAUNCHER_TITLE_ZH_HANT; text = LAUNCHER_PYTHON_MISSING_ZH_HANT;
    } else {
        for (size_t i = 0; i < sizeof by_primary / sizeof by_primary[0]; i++)
            if (PRIMARYLANGID(ui) == by_primary[i].primary) { title = by_primary[i].title; text = by_primary[i].text; }
    }
    MessageBoxW(NULL, text, title, MB_OK | MB_ICONERROR);
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
