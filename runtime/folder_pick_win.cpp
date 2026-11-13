// The Win32 front end of folder_pick.h: the shell's folder dialog (IFileOpenDialog with FOS_PICKFOLDERS), owned by the
// game window, on a thread of its own so the present thread that draws the menu keeps running.
#include <windows.h>
#include <shobjidl.h>
#include <stdlib.h>
#include <string>

extern "C" {
#include "folder_pick.h"
#include "host.h"
#include "input_win.h"
#include "platform/platform.h"
}

struct PickReq {
    std::string start;
    FolderPickDone done;
    void *ctx;
};

static std::wstring wide(const std::string &utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);
    std::wstring w(n > 0 ? n : 1, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], n);
    w.resize(wcslen(w.c_str()));
    return w;
}

static std::string utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    std::string s(n > 0 ? n : 1, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, NULL, NULL);
    s.resize(strlen(s.c_str()));
    return s;
}

// The dialog wants an absolute folder that exists; anything else leaves it at the shell's last folder.
static void set_start(IFileOpenDialog *d, const std::string &start) {
    wchar_t full[MAX_PATH * 2];
    if (start.empty() || !GetFullPathNameW(wide(start).c_str(), MAX_PATH * 2, full, NULL)) return;
    IShellItem *item = NULL;
    if (SUCCEEDED(SHCreateItemFromParsingName(full, NULL, IID_PPV_ARGS(&item)))) {
        d->SetFolder(item);
        item->Release();
    }
}

static void pick_main(void *arg) {
    PickReq *r = (PickReq *)arg;
    std::string chosen;
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog *d = NULL;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d));
    if (SUCCEEDED(hr)) {
        FILEOPENDIALOGOPTIONS opt = 0;
        d->GetOptions(&opt);
        d->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        set_start(d, r->start);
        hr = d->Show(gfx_window_handle());
        IShellItem *item = NULL;
        if (SUCCEEDED(hr) && SUCCEEDED(hr = d->GetResult(&item))) {
            PWSTR path = NULL;
            if (SUCCEEDED(hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                chosen = utf8(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
        d->Release();
    }
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) rt_log("folder_pick: the folder dialog failed (0x%08lx)", (unsigned long)hr);
    r->done(chosen.empty() ? NULL : chosen.c_str(), r->ctx);
    if (SUCCEEDED(co)) CoUninitialize();
    delete r;
}

extern "C" void folder_pick(const char *start, FolderPickDone done, void *ctx) {
    PickReq *r = new PickReq{start ? start : "", done, ctx};
    if (!plat_thread_spawn(pick_main, r, "folder dialog", 0)) {
        rt_log("folder_pick: cannot start the dialog thread");
        done(NULL, ctx);
        delete r;
    }
}
