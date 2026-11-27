// totk-config: updates from the project's GitHub releases (the Updates tab, --check-update, --download-update). The
// latest release's Windows setup is downloaded, checked against the size and SHA-256 GitHub lists for it, unpacked, and
// its setup window is started on the game folder, which updates the install as running the new release's
// TotK-Setup.exe on it would: saves, game files and totk.ini stay; totk.exe and the shaders are rebuilt.
//
// - Check: GET https://api.github.com/repos/valentderah/totk-pc-recomp/releases/latest (GitHub leaves drafts and
//   pre-releases out of it), its TotK-Setup-<version>-windows.zip asset with "size" and "digest" ("sha256:..."). The
//   asset must come from this repository's download address.
// - Download: into %LOCALAPPDATA%\TotK Recomp\updates\ as <name>.part, renamed once size and SHA-256 match; a mismatch
//   deletes it.
// - Unpack: Windows' own tar.exe (bsdtar reads zip) after a listing that refuses absolute paths and "..". The zip's one
//   folder becomes updates\TotK-Setup-<version>, and its VERSION must name the release.
// - Install: <release>\python\pythonw.exe -m installer.gui --root <release> --rebuild <game folder> (--upgrade when
//   the release's setup knows it): the setup's run on that folder starts at once, as the game's own rebuild starts it,
//   and the game starts when it succeeds. Never while the folder's totk.exe runs.
//
// The version installed: logs/totk.log names the totk.exe that ran last ("version: TotK Recomp 0.5.0+..."), and
// totk-install.json the release whose setup made the folder. A log older than that record comes from before the setup
// ran and is not read. When the two differ, or totk.exe is newer than the record, the folder holds a build of its own,
// which an update replaces; the window says so first.
//
// Network, hashing and unpacking are Windows-only (WinHTTP, BCrypt, tar.exe): the releases' setups are Windows
// programs. Elsewhere the tab is not shown and the commands say so. TOTK_CONFIG_UPDATE_API names another API address
// for tests (only http://127.0.0.1 or http://localhost), TOTK_CONFIG_UPDATES_DIR another download folder.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "configurator/cfgr.h"
#include "platform/platform.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#endif

#define REPO "valentderah/totk-pc-recomp"
#define API "https://api.github.com"

// ---- versions
int upd_parse_version(const char *s, int v[3]) {
    while (*s == ' ' || *s == 'v' || *s == 'V') s++;
    int n = 0;
    v[0] = v[1] = v[2] = 0;
    for (;;) {
        if (*s < '0' || *s > '9' || n == 3) return 0;
        long x = strtol(s, (char **)&s, 10);
        v[n++] = (int)x;
        if (*s != '.') break;
        s++;
    }
    if (n < 2) return 0;
    return *s == 0 || *s == '+' || *s == '-' || *s == ' ' || *s == '\r' || *s == '\n';
}

int upd_version_cmp(const int a[3], const int b[3]) {
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

// ---- a small JSON reader: enough for GitHub's release answer and the window's own settings file
static const char *ws(const char *p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static const char *skip_string(const char *p) {   // p at the opening quote
    for (p++; *p; p++) {
        if (*p == '\\') {
            if (!*++p) return NULL;
        } else if (*p == '"')
            return p + 1;
    }
    return NULL;
}

static const char *skip_value(const char *p) {
    p = ws(p);
    if (!p || !*p) return NULL;
    if (*p == '"') return skip_string(p);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = ws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (close == '}') {
                if (*p != '"' || !(p = skip_string(p))) return NULL;
                p = ws(p);
                if (*p != ':') return NULL;
                p++;
            }
            if (!(p = skip_value(p))) return NULL;
            p = ws(p);
            if (*p == ',') p = ws(p + 1);
            else if (*p == close) return p + 1;
            else return NULL;
        }
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\r' && *p != '\n' && *p != '\t') p++;
    return p;
}

// the value of key in the object at obj ('{'), or NULL
static const char *obj_get(const char *obj, const char *key) {
    const char *p = ws(obj);
    if (!p || *p != '{') return NULL;
    p = ws(p + 1);
    size_t kn = strlen(key);
    while (*p == '"') {
        const char *k = p + 1, *e = skip_string(p);
        if (!e) return NULL;
        int match = (size_t)(e - 1 - k) == kn && !memcmp(k, key, kn);
        p = ws(e);
        if (*p != ':') return NULL;
        p = ws(p + 1);
        if (match) return p;
        if (!(p = skip_value(p))) return NULL;
        p = ws(p);
        if (*p == ',') p = ws(p + 1);
    }
    return NULL;
}

static void put_utf8(char **o, char *end, unsigned cp) {
    char b[4];
    int n = cp < 0x80 ? (b[0] = (char)cp, 1)
          : cp < 0x800 ? (b[0] = (char)(0xC0 | cp >> 6), b[1] = (char)(0x80 | (cp & 63)), 2)
          : cp < 0x10000 ? (b[0] = (char)(0xE0 | cp >> 12), b[1] = (char)(0x80 | (cp >> 6 & 63)), b[2] = (char)(0x80 | (cp & 63)), 3)
                         : (b[0] = (char)(0xF0 | cp >> 18), b[1] = (char)(0x80 | (cp >> 12 & 63)),
                            b[2] = (char)(0x80 | (cp >> 6 & 63)), b[3] = (char)(0x80 | (cp & 63)), 4);
    for (int i = 0; i < n && *o < end; i++) *(*o)++ = b[i];
}

// A JSON string decoded into a new buffer (free it); NULL when v is not a string.
static char *str_dup(const char *v) {
    v = ws(v);
    if (!v || *v != '"') return NULL;
    const char *e = skip_string(v);
    if (!e) return NULL;
    size_t cap = (size_t)(e - v) + 1;
    char *out = malloc(cap), *o = out, *end = out + cap - 1;
    for (const char *p = v + 1; p < e - 1; p++) {
        if (*p != '\\') {
            if (o < end) *o++ = *p;
            continue;
        }
        p++;
        switch (*p) {
        case 'n': if (o < end) *o++ = '\n'; break;
        case 't': if (o < end) *o++ = '\t'; break;
        case 'r': break;
        case 'b': case 'f': break;
        case 'u': {
            unsigned cp = (unsigned)strtoul((char[5]){p[1], p[2], p[3], p[4], 0}, NULL, 16);
            p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && p[1] == '\\' && p[2] == 'u') {
                unsigned lo = (unsigned)strtoul((char[5]){p[3], p[4], p[5], p[6], 0}, NULL, 16);
                if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00), p += 6;
            }
            put_utf8(&o, end, cp);
            break;
        }
        default: if (o < end) *o++ = *p; break;   // \" \\ \/
        }
    }
    *o = 0;
    return out;
}

static int str_get(const char *v, char *out, size_t cap) {
    char *s = str_dup(v);
    if (!s) return 0;
    snprintf(out, cap, "%s", s);
    free(s);
    return 1;
}

static int bool_get(const char *v) { return v && !strncmp(ws(v), "true", 4); }

// ---- the release answer
static const char *api_base(void) {
    static char base[256];
    const char *e = getenv("TOTK_CONFIG_UPDATE_API");
    if (e && (!strncmp(e, "http://127.0.0.1:", 17) || !strncmp(e, "http://localhost:", 17))) {
        snprintf(base, sizeof base, "%s", e);
        size_t n = strlen(base);
        while (n && base[n - 1] == '/') base[--n] = 0;
        return base;
    }
    return API;
}

// where a release asset may come from: this repository's download address, or the test API's host
static int allowed_download(const char *url, const char *base) {
    const char *gh = "https://github.com/" REPO "/releases/download/";
    if (!strncmp(url, gh, strlen(gh))) return 1;
    size_t n = strlen(base);
    return strcmp(base, API) && !strncmp(url, base, n) && url[n] == '/';
}

int upd_parse_release(const char *json, const char *base, CrRelease *r, char *err, size_t en) {
    memset(r, 0, sizeof *r);
    if (!base) base = api_base();
    const char *v = obj_get(json, "tag_name");
    if (!v || !str_get(v, r->tag, sizeof r->tag)) return snprintf(err, en, "unexpected answer from GitHub"), 0;
    if (bool_get(obj_get(json, "draft")) || bool_get(obj_get(json, "prerelease")))
        return snprintf(err, en, "%s is a draft or a pre-release", r->tag), 0;
    if (!upd_parse_version(r->tag, r->ver)) return snprintf(err, en, "no version number in the tag %s", r->tag), 0;
    if (!(v = obj_get(json, "name")) || !str_get(v, r->name, sizeof r->name) || !r->name[0])
        snprintf(r->name, sizeof r->name, "%s", r->tag);
    if ((v = obj_get(json, "html_url"))) str_get(v, r->page, sizeof r->page);
    if ((v = obj_get(json, "published_at"))) str_get(v, r->published, sizeof r->published);
    r->notes = (v = obj_get(json, "body")) ? str_dup(v) : NULL;
    const char *a = ws(obj_get(json, "assets"));
    int found = 0;
    if (a && *a == '[') {
        for (a = ws(a + 1); *a == '{'; ) {
            char name[160] = "";
            if ((v = obj_get(a, "name"))) str_get(v, name, sizeof name);
            size_t nl = strlen(name);
            if (!found && !strncmp(name, "TotK-Setup-", 11) && nl > 12 && !strcmp(name + nl - 12, "-windows.zip")) {
                found = 1;
                snprintf(r->asset, sizeof r->asset, "%s", name);
                if ((v = obj_get(a, "size"))) r->size = strtoull(ws(v), NULL, 10);
                char digest[128] = "";
                if ((v = obj_get(a, "digest"))) str_get(v, digest, sizeof digest);
                if (!strncmp(digest, "sha256:", 7) && strlen(digest + 7) == 64 &&
                    strspn(digest + 7, "0123456789abcdefABCDEF") == 64)
                    for (int i = 0; i < 64; i++) r->sha256[i] = (char)SDL_tolower(digest[7 + i]);
                if ((v = obj_get(a, "browser_download_url"))) str_get(v, r->url, sizeof r->url);
            }
            if (!(a = skip_value(a))) break;
            a = ws(a);
            if (*a == ',') a = ws(a + 1);
        }
    }
    if (!found) return snprintf(err, en, "%s has no Windows setup (TotK-Setup-*-windows.zip)", r->tag), 0;
    if (!r->sha256[0]) return snprintf(err, en, "%s lists no SHA-256 for %s", r->tag, r->asset), 0;
    if (!allowed_download(r->url, base))
        return snprintf(err, en, "%s would download from %s, not from this project", r->asset, r->url), 0;
    if (strchr(r->asset, '/') || strchr(r->asset, '\\') || strstr(r->asset, ".."))
        return snprintf(err, en, "unexpected asset name %s", r->asset), 0;
    return 1;
}

void upd_release_free(CrRelease *r) {
    free(r->notes);
    r->notes = NULL;
}

// ---- the version installed
static int mtime_of(const char *path, uint64_t *out) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return 0;
    *out = (uint64_t)d.ftLastWriteTime.dwHighDateTime << 32 | d.ftLastWriteTime.dwLowDateTime;   // 100 ns units
    return 1;
#else
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return 0;
    *out = (uint64_t)(info.modify_time / 100);
    return 1;
#endif
}

static void join(char *out, size_t cap, const char *a, const char *b) {
    size_t n = strlen(a);
    snprintf(out, cap, "%s%s%s", a, n && (a[n - 1] == '/' || a[n - 1] == '\\') ? "" : "/", b);
}

int upd_version_in_log(const char *text, char *out, size_t cap) {
    const char *k = "version: TotK Recomp ", *p = strstr(text, k);
    if (!p) return 0;
    p += strlen(k);
    size_t n = strcspn(p, ", \r\n");
    if (!n) return 0;
    snprintf(out, cap, "%.*s", (int)n, p);
    return 1;
}

void upd_installed(const char *dir, CrInstalled *in) {
    memset(in, 0, sizeof *in);
    char log[CR_PATH], rec[CR_PATH], exe[CR_PATH];
    join(log, sizeof log, dir, "logs/totk.log");
    join(rec, sizeof rec, dir, "totk-install.json");
    join(exe, sizeof exe, dir, cr_game_exe);
    uint64_t tl = 0, tr = 0, te = 0;
    int has_log = mtime_of(log, &tl), has_rec = mtime_of(rec, &tr), has_exe = mtime_of(exe, &te);
    // a log written before the setup's record comes from the build the setup replaced
    if (has_log && !(has_rec && tl < tr)) {
        uint64_t n = 0;
        char *t = (char *)plat_file_read(log, &n);
        if (t) {
            char *z = realloc(t, n + 1);
            if (z) t = z, t[n] = 0, upd_version_in_log(t, in->run, sizeof in->run);
            free(t);
        }
    }
    if (has_rec) {
        uint64_t n = 0;
        char *t = (char *)plat_file_read(rec, &n);
        if (t) {
            char *z = realloc(t, n + 1);
            if (z) {
                t = z, t[n] = 0;
                const char *v = obj_get(t, "release");
                if (v) str_get(v, in->setup, sizeof in->setup);
            }
            free(t);
        }
    }
    // the setup writes totk-install.json after it links totk.exe; a totk.exe more than a minute newer came later
    int exe_newer = has_exe && has_rec && te > tr + 600000000ull;
    int differs = in->run[0] && in->setup[0] && strcmp(in->run, in->setup);
    in->custom = in->setup[0] && (differs || exe_newer);
    in->have = upd_parse_version(in->run[0] ? in->run : in->setup, in->ver);
}

// ---- the window's own settings: configurator.json in the per-user folder the setup keeps its state in
static void state_dir(char *out, size_t cap) {
#ifdef _WIN32
    const char *l = getenv("LOCALAPPDATA");
    snprintf(out, cap, "%s\\TotK Recomp", l ? l : ".");
#else
    const char *x = getenv("XDG_STATE_HOME"), *h = getenv("HOME");
    if (x && *x) snprintf(out, cap, "%s/TotK Recomp", x);
    else snprintf(out, cap, "%s/.local/state/TotK Recomp", h ? h : ".");
#endif
}

// On unless the player turned it off: no file, or a file without the key, means on.
int upd_pref_check_at_start(void) {
    char d[CR_PATH], p[CR_PATH];
    state_dir(d, sizeof d);
    join(p, sizeof p, d, "configurator.json");
    uint64_t n = 0;
    char *t = (char *)plat_file_read(p, &n);
    if (!t) return 1;
    char *z = realloc(t, n + 1);
    int on = 1;
    if (z) {
        t = z, t[n] = 0;
        const char *v = obj_get(t, "check_updates");
        if (v) on = bool_get(v);
    }
    free(t);
    return on;
}

int upd_pref_set_check_at_start(int on) {
    char d[CR_PATH], p[CR_PATH], text[64];
    state_dir(d, sizeof d);
    plat_file_mkdir_p(d);
    join(p, sizeof p, d, "configurator.json");
    int n = snprintf(text, sizeof text, "{\n  \"check_updates\": %s\n}", on ? "true" : "false");
    return plat_file_write(p, text, (size_t)n);
}

// ---- the worker's state, shared with the window
static SDL_Mutex *g_lock;
static CrUpdState g_st;
static CrRelease g_latest;
static int g_have_latest;

static void lock(void) {
    if (!g_lock) g_lock = SDL_CreateMutex();
    SDL_LockMutex(g_lock);
}
static void unlock(void) { SDL_UnlockMutex(g_lock); }

static void set_phase(int phase, const char *msg) {
    lock();
    g_st.phase = phase;
    if (msg) snprintf(g_st.msg, sizeof g_st.msg, "%s", msg);
    unlock();
}

void upd_state(CrUpdState *out) {
    lock();
    *out = g_st;
    unlock();
}

int upd_latest(CrRelease *out) {   // a copy without the notes pointer's ownership: the notes stay the module's
    lock();
    int have = g_have_latest;
    if (have) *out = g_latest;
    unlock();
    return have;
}

int upd_busy(void) {
    CrUpdState s;
    upd_state(&s);
    return s.phase == UPD_CHECKING || s.phase == UPD_DOWNLOADING || s.phase == UPD_VERIFYING || s.phase == UPD_UNPACKING;
}

#ifdef _WIN32
// ---- HTTPS through WinHTTP (the system's TLS and certificate store); redirects (GitHub's downloads) are followed
typedef int (*Sink)(void *ctx, const void *data, size_t n);

static int http_get(const char *url, const char *accept, int timeout_ms, Sink sink, void *ctx, char *err, size_t en) {
    wchar_t wurl[2048], whost[256], wpath[2048], whdr[256];
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048)) return snprintf(err, en, "bad address %s", url), 0;
    URL_COMPONENTS uc = {sizeof uc};
    uc.lpszHostName = whost, uc.dwHostNameLength = 256;
    uc.lpszUrlPath = wpath, uc.dwUrlPathLength = 2048;
    wchar_t extra[1024];
    uc.lpszExtraInfo = extra, uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return snprintf(err, en, "bad address %s", url), 0;
    if (uc.dwExtraInfoLength && wcslen(wpath) + wcslen(extra) < 2048) wcscat(wpath, extra);
    HINTERNET s = WinHttpOpen(L"totk-config", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!s) s = WinHttpOpen(L"totk-config", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    HINTERNET c = s ? WinHttpConnect(s, whost, uc.nPort, 0) : NULL;
    int secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", wpath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         secure ? WINHTTP_FLAG_SECURE : 0)
                    : NULL;
    int ok = 0;
    if (r) {
        WinHttpSetTimeouts(s, 20000, 20000, timeout_ms, timeout_ms);
        swprintf(whdr, 256, L"Accept: %hs", accept);
        WinHttpAddRequestHeaders(r, whdr, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(r, NULL)) {
            DWORD code = 0, sz = sizeof code;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &code, &sz, WINHTTP_NO_HEADER_INDEX);
            if (code != 200) snprintf(err, en, "%s answered HTTP %lu", url, (unsigned long)code);
            else {
                static char buf[1 << 18];
                ok = 1;
                for (;;) {
                    DWORD got = 0;
                    if (!WinHttpReadData(r, buf, sizeof buf, &got)) {
                        snprintf(err, en, "the connection broke off (error %lu)", (unsigned long)GetLastError());
                        ok = 0;
                        break;
                    }
                    if (!got) break;
                    if (!sink(ctx, buf, got)) {
                        ok = 0;
                        break;
                    }
                }
            }
        } else
            snprintf(err, en, "could not reach %ls (error %lu)", whost, (unsigned long)GetLastError());
    } else
        snprintf(err, en, "could not reach %ls (error %lu)", whost, (unsigned long)GetLastError());
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

typedef struct { char *p; size_t n, cap; } Buf;
static int buf_sink(void *ctx, const void *data, size_t n) {
    Buf *b = ctx;
    if (b->n + n + 1 > 8u << 20) return 0;   // an API answer is small
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
    b->p[b->n] = 0;
    return 1;
}

int upd_check(CrRelease *out, char *err, size_t en) {
    char url[512];
    const char *base = api_base();
    snprintf(url, sizeof url, "%s/repos/" REPO "/releases/latest", base);
    Buf b = {0};
    int ok = http_get(url, "application/vnd.github+json", 30000, buf_sink, &b, err, en) && b.p &&
             upd_parse_release(b.p, base, out, err, en);
    free(b.p);
    return ok;
}

// ---- the download
static int sha256_file(const char *path, char hex[65]) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    int ok = 0;
    FILE *f = fopen(path, "rb");
    if (f && BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
        BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        static unsigned char buf[1 << 20];
        size_t n;
        ok = 1;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            if (BCryptHashData(h, buf, (ULONG)n, 0)) ok = 0;
        unsigned char d[32];
        if (ok && BCryptFinishHash(h, d, 32, 0) == 0)
            for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
        else
            ok = 0;
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (f) fclose(f);
    return ok;
}

static int verify(const char *path, const CrRelease *r, char *err, size_t en) {
    int64_t size = plat_file_size(path);
    if (size != (int64_t)r->size)
        return snprintf(err, en, "%s: %lld bytes, GitHub lists %llu", r->asset, (long long)size,
                        (unsigned long long)r->size), 0;
    char hex[65] = "";
    if (!sha256_file(path, hex)) return snprintf(err, en, "%s: could not read it back", r->asset), 0;
    if (strcmp(hex, r->sha256))
        return snprintf(err, en, "%s: SHA-256 %.16s, GitHub lists %.16s", r->asset, hex, r->sha256), 0;
    return 1;
}

typedef struct { FILE *f; uint64_t done, total; int over; } Dl;
static int dl_sink(void *ctx, const void *data, size_t n) {
    Dl *d = ctx;
    d->done += n;
    if (d->done > d->total) return d->over = 1, 0;
    if (fwrite(data, 1, n, d->f) != n) return 0;
    lock();
    g_st.done = d->done;
    unlock();
    return 1;
}

static void updates_dir(char *out, size_t cap) {
    const char *e = getenv("TOTK_CONFIG_UPDATES_DIR");
    if (e && *e) {
        snprintf(out, cap, "%s", e);
        return;
    }
    char d[CR_PATH];
    state_dir(d, sizeof d);
    join(out, cap, d, "updates");
}

static int download(const CrRelease *r, const char *dir, char *path, size_t pcap, char *err, size_t en) {
    plat_file_mkdir_p(dir);
    join(path, pcap, dir, r->asset);
    if (plat_file_exists(path)) {   // a finished download from before, when it still matches
        set_phase(UPD_VERIFYING, NULL);
        if (verify(path, r, err, en)) return 1;
        plat_file_delete(path);
    }
    char part[CR_PATH];
    snprintf(part, sizeof part, "%s.part", path);
    Dl d = {fopen(part, "wb"), 0, r->size, 0};
    if (!d.f) return snprintf(err, en, "%s: cannot write there", part), 0;
    lock();
    g_st.phase = UPD_DOWNLOADING, g_st.done = 0, g_st.total = r->size;
    unlock();
    int ok = http_get(r->url, "application/octet-stream", 60000, dl_sink, &d, err, en);
    if (fclose(d.f)) ok = 0;
    if (d.over) snprintf(err, en, "%s is longer than the %llu bytes GitHub lists", r->asset, (unsigned long long)r->size);
    if (ok) {
        set_phase(UPD_VERIFYING, NULL);
        ok = verify(part, r, err, en) && plat_file_replace(part, path);
    }
    if (!ok) plat_file_delete(part);
    return ok;
}

// ---- unpacking with Windows' own tar.exe (by full path, so a tar.exe elsewhere on PATH never runs)
static int run_tar(const char *args, const char *out_file) {
    char tar[MAX_PATH], cmd[4 * CR_PATH];
    const char *root = getenv("SystemRoot");
    snprintf(tar, sizeof tar, "%s\\System32\\tar.exe", root ? root : "C:\\Windows");
    snprintf(cmd, sizeof cmd, "\"%s\" %s", tar, args);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE out = INVALID_HANDLE_VALUE;
    if (out_file) out = CreateFileA(out_file, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, NULL);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (out != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = out;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    int ok = CreateProcessA(tar, cmd, NULL, NULL, out != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    DWORD code = 1;
    if (ok) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    return ok && code == 0;
}

int upd_safe_entry(const char *name) {
    if (!*name || *name == '/' || *name == '\\' || (name[0] && name[1] == ':')) return 0;
    for (const char *p = name; *p;) {
        size_t n = strcspn(p, "/\\");
        if (n == 2 && p[0] == '.' && p[1] == '.') return 0;
        p += n;
        if (*p) p++;
    }
    return 1;
}

static void remove_tree(const char *dir) {
    char pat[CR_PATH];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            char p[CR_PATH];
            snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                remove_tree(p);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveDirectoryA(p);
            else {
                SetFileAttributesA(p, FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(p);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

static int release_ok(const char *dir, const CrRelease *r, char *err, size_t en) {
    char p[CR_PATH];
    join(p, sizeof p, dir, "VERSION");
    uint64_t n = 0;
    char *t = (char *)plat_file_read(p, &n), v[64] = "";
    if (!t) return snprintf(err, en, "%s has no VERSION", dir), 0;
    snprintf(v, sizeof v, "%.*s", (int)(n < 63 ? n : 63), t);
    free(t);
    v[strcspn(v, "\r\n")] = 0;
    int ver[3];
    if (!upd_parse_version(v, ver) || upd_version_cmp(ver, r->ver))
        return snprintf(err, en, "the unpacked release says %s, not %s", v, r->tag), 0;
    static const char *const need[] = {"python/pythonw.exe", "installer/gui.py"};
    for (int i = 0; i < 2; i++) {
        join(p, sizeof p, dir, need[i]);
        if (!plat_file_exists(p)) return snprintf(err, en, "the unpacked release lacks %s", need[i]), 0;
    }
    return 1;
}

static int unpack(const char *zip, const CrRelease *r, const char *dir, char *target, size_t tcap, char *err, size_t en) {
    snprintf(target, tcap, "%s\\TotK-Setup-%d.%d.%d", dir, r->ver[0], r->ver[1], r->ver[2]);
    char dummy[256];
    if (release_ok(target, r, dummy, sizeof dummy)) return 1;
    char list[CR_PATH], args[3 * CR_PATH];
    snprintf(list, sizeof list, "%s\\list.txt", dir);
    snprintf(args, sizeof args, "-tf \"%s\"", zip);
    if (!run_tar(args, list)) return snprintf(err, en, "%s is not a readable zip", r->asset), 0;
    uint64_t n = 0;
    char *names = (char *)plat_file_read(list, &n);
    plat_file_delete(list);
    if (!names) return snprintf(err, en, "%s: no listing", r->asset), 0;
    char *z = realloc(names, n + 1);
    if (!z) return free(names), 0;
    names = z, names[n] = 0;
    for (char *line = strtok(names, "\r\n"); line; line = strtok(NULL, "\r\n"))
        if (!upd_safe_entry(line)) {
            snprintf(err, en, "%s holds an entry outside its folder: %s", r->asset, line);
            free(names);
            return 0;
        }
    free(names);
    char tmp[CR_PATH];
    snprintf(tmp, sizeof tmp, "%s.unpack", target);
    remove_tree(tmp);
    plat_file_mkdir_p(tmp);
    snprintf(args, sizeof args, "-xf \"%s\" -C \"%s\"", zip, tmp);
    if (!run_tar(args, NULL)) {
        remove_tree(tmp);
        return snprintf(err, en, "could not unpack %s", r->asset), 0;
    }
    // the zip holds one folder (tools/make_release.py: TotK-Setup/...)
    char inner[CR_PATH], pat[CR_PATH];
    snprintf(inner, sizeof inner, "%s", tmp);
    snprintf(pat, sizeof pat, "%s\\*", tmp);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char p[CR_PATH], vp[CR_PATH];
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            snprintf(p, sizeof p, "%s\\%s", tmp, fd.cFileName);
            join(vp, sizeof vp, p, "VERSION");
            if (plat_file_exists(vp)) {
                snprintf(inner, sizeof inner, "%s", p);
                break;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    remove_tree(target);
    if (!MoveFileExA(inner, target, MOVEFILE_WRITE_THROUGH)) {
        remove_tree(tmp);
        return snprintf(err, en, "%s: cannot move the release there (error %lu)", target, (unsigned long)GetLastError()), 0;
    }
    remove_tree(tmp);
    if (!release_ok(target, r, err, en)) {
        remove_tree(target);
        return 0;
    }
    plat_file_delete(zip);   // the folder is what the setup runs from
    return 1;
}

int upd_download(const CrRelease *r, char *folder, size_t fcap, char *err, size_t en) {
    char dir[CR_PATH], zip[CR_PATH];
    updates_dir(dir, sizeof dir);
    if (!download(r, dir, zip, sizeof zip, err, en)) return 0;
    set_phase(UPD_UNPACKING, NULL);
    return unpack(zip, r, dir, folder, fcap, err, en);
}

int upd_game_running(const char *dir) {
    char exe[CR_PATH];
    join(exe, sizeof exe, dir, cr_game_exe);
    if (!plat_file_exists(exe)) return 0;
    HANDLE h = CreateFileA(exe, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return 0;
    }
    DWORD e = GetLastError();
    return e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED;
}

int upd_start_setup(const char *release, const char *game, char *err, size_t en) {
    char gui[CR_PATH], py[CR_PATH], cmd[4 * CR_PATH], rel[CR_PATH], dst[CR_PATH];
    snprintf(rel, sizeof rel, "%s", release);
    snprintf(dst, sizeof dst, "%s", game);
    // a trailing separator would escape the closing quote of the command line
    for (size_t n = strlen(rel); n > 3 && (rel[n - 1] == '\\' || rel[n - 1] == '/'); n--) rel[n - 1] = 0;
    for (size_t n = strlen(dst); n > 3 && (dst[n - 1] == '\\' || dst[n - 1] == '/'); n--) dst[n - 1] = 0;
    join(gui, sizeof gui, rel, "installer/gui.py");
    join(py, sizeof py, rel, "python/pythonw.exe");
    for (char *p = py; *p; p++)
        if (*p == '/') *p = '\\';
    uint64_t n = 0;
    char *text = (char *)plat_file_read(gui, &n);
    int upgrade = 0;
    if (text) {
        char *z = realloc(text, n + 1);
        if (z) text = z, text[n] = 0, upgrade = strstr(text, "'--upgrade'") != NULL;
        free(text);
    }
    snprintf(cmd, sizeof cmd, "\"%s\" -m installer.gui --root \"%s\" %s \"%s\"", py, rel,
             upgrade ? "--upgrade" : "--rebuild", dst);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(py, cmd, NULL, NULL, FALSE, 0, NULL, rel, &si, &pi))
        return snprintf(err, en, "could not start %s (error %lu)", py, (unsigned long)GetLastError()), 0;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 1;
}
#else
int upd_check(CrRelease *out, char *err, size_t en) {
    (void)out;
    return snprintf(err, en, "updates are for the Windows setup; update the game the way you built it"), 0;
}
int upd_download(const CrRelease *r, char *folder, size_t fcap, char *err, size_t en) {
    (void)r, (void)folder, (void)fcap;
    return snprintf(err, en, "updates are for the Windows setup"), 0;
}
int upd_game_running(const char *dir) {
    (void)dir;
    return 0;
}
int upd_start_setup(const char *release, const char *game, char *err, size_t en) {
    (void)release, (void)game;
    return snprintf(err, en, "updates are for the Windows setup"), 0;
}
int upd_safe_entry(const char *name) { return name && *name && *name != '/' && !strstr(name, ".."); }
#endif

// ---- the worker threads
static int SDLCALL check_thread(void *arg) {
    (void)arg;
    CrRelease r;
    char err[512] = "";
    if (upd_check(&r, err, sizeof err)) {
        lock();
        g_latest = r, g_have_latest = 1;   // the earlier answer's notes stay allocated: the window may hold them
        g_st.phase = UPD_CHECKED;
        unlock();
    } else
        set_phase(UPD_FAILED, err);
    return 0;
}

void upd_check_async(void) {
    if (upd_busy()) return;
    set_phase(UPD_CHECKING, "");
    SDL_Thread *t = SDL_CreateThread(check_thread, "update check", NULL);
    if (t) SDL_DetachThread(t);
    else set_phase(UPD_FAILED, "could not start a thread");
}

static int SDLCALL fetch_thread(void *arg) {
    (void)arg;
    CrRelease r;
    if (!upd_latest(&r)) return set_phase(UPD_FAILED, "no release checked"), 0;
    char folder[CR_PATH], err[512] = "";
    if (upd_download(&r, folder, sizeof folder, err, sizeof err)) {
        lock();
        g_st.phase = UPD_READY;
        snprintf(g_st.folder, sizeof g_st.folder, "%s", folder);
        unlock();
    } else
        set_phase(UPD_FAILED, err);
    return 0;
}

void upd_fetch_async(void) {
    if (upd_busy()) return;
    set_phase(UPD_DOWNLOADING, "");
    SDL_Thread *t = SDL_CreateThread(fetch_thread, "update download", NULL);
    if (t) SDL_DetachThread(t);
    else set_phase(UPD_FAILED, "could not start a thread");
}

void upd_set_failed(const char *why) { set_phase(UPD_FAILED, why); }
