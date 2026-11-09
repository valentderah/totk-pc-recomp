// The in-game update (update.h, Windows): the Menu tab of the settings menu says when GitHub has a newer release of
// the port than the running one, downloads its Windows setup on the player's OK and starts it on this install folder
// once the game closes. The setup then runs as an update from its window does (docs/installer.md): it rebuilds
// totk.exe, prepares the shaders again, keeps the saves, game files and totk.ini, and starts the game.
//
// The check: one GET of the project's latest release (GitHub's releases/latest, which skips drafts and pre-releases)
// the first time the menu opens in a run, while TOTK_UPDATE_CHECK is on; "Check now" runs one while it is off. Its tag
// ("v0.3.13") is compared with rt_port_version ("0.3.12+eda417f") by the three version numbers; a running pre-release
// ("0.3.13-dev") is older than its release.
//
// The download: the release's TotK-Setup-<version>-windows.zip only, from the project's download URL, into
// %LOCALAPPDATA%\TotK Recomp\updates\. Its size and SHA-256 must match the ones the release answer lists for it (the
// asset's "digest"). tar.exe unpacks it (plat_unzip), the zip goes, and the unpacked VERSION must name the release. A
// finished folder gets the file .totk-update-ok, and a later run that finds it skips the download (a game started
// again before the update ran, a setup run that failed and resumes). What this does not cover: releases are not
// signed, so someone who took over the project's GitHub account could publish a setup that passes these checks.
//
// The install: plat_spawn starts the release's python\pythonw.exe -m installer.gui --upgrade <this folder> (the setup
// window's update mode, installer/gui.py: it waits until totk.exe is free, updates the folder, starts the game when
// it succeeds and deletes the release folders an earlier update left in updates\); then the menu closes the game as
// Quit does. A folder without totk-install.json (a build the setup did not make) and a scripted run (--seconds,
// --boot, TOTK_MENU_TEST) never install, and a scripted run never checks by itself.
//
// TOTK_UPDATE_API (test hook): the releases/latest URL of a test server on this machine (http://127.0.0.1:<port>/...
// or http://localhost:<port>/...); the download URLs its answer lists must then point at that server.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "sha256.h"
#include "update.h"

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

#define REPO "valentderah/totk-pc-recomp"
#define API_URL "https://api.github.com/repos/" REPO "/releases/latest"
#define PAGE_PREFIX "https://github.com/" REPO "/releases/"
#define DOWNLOAD_PREFIX "https://github.com/" REPO "/releases/download/"
#define API_HEADERS "Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n"
#define ANSWER_CAP (1u << 20)        // the release answer: a few KiB of JSON with the release notes
#define SETUP_MAX (2ull << 30)       // no setup of ours comes near this (0.3.13: 157 MiB)
#define UNZIP_MS (10u * 60 * 1000)   // tar.exe on a hard drive
#define OK_MARK ".totk-update-ok"

// ---- versions

// The three numbers that start s ("0.3.12+eda417f", "0.3.13-dev+1a2b3c4") into v, and in *pre whether a pre-release
// part ('-') follows them. 0 when s does not start with three numbers and an end, '-' or '+'.
static int parse_version(const char *s, unsigned v[3], int *pre) {
    for (int i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9') return 0;
        unsigned n = 0;
        while (*s >= '0' && *s <= '9') {
            if (n > 99999) return 0;
            n = n * 10 + (unsigned)(*s++ - '0');
        }
        v[i] = n;
        if (i < 2 && *s++ != '.') return 0;
    }
    if (*s && *s != '-' && *s != '+') return 0;
    *pre = *s == '-';
    return 1;
}

// 1 when release (a release version: no pre-release part) is newer than running, 0 when not, -1 when either is not a
// version. A running pre-release of the same numbers ("0.3.13-dev") is older than the release.
static int version_newer(const char *release, const char *running) {
    unsigned a[3], b[3];
    int apre, bpre;
    if (!parse_version(release, a, &apre) || apre || !parse_version(running, b, &bpre)) return -1;
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] > b[i];
    return bpre;
}

// ---- the release answer: a JSON reader for GitHub's release object, one pass, no tree. Strings come out decoded
// (\uXXXX as UTF-8, a surrogate pair joined); a malformed answer fails as a whole (bad).
typedef struct { const char *p, *end; int bad; } Js;

static int js_peek(Js *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
    return j->p < j->end ? (unsigned char)*j->p : -1;
}

static int js_eat(Js *j, char c) {
    if (js_peek(j) != (unsigned char)c) return 0;
    j->p++;
    return 1;
}

static int js_hex4(Js *j, unsigned *out) {
    if (j->end - j->p < 4) return 0;
    unsigned u = 0;
    for (int i = 0; i < 4; i++) {
        char c = *j->p++;
        u = u << 4 | (unsigned)(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99);
        if (u & ~0xFFFFu) return 0;
    }
    *out = u;
    return 1;
}

// The string at the cursor into out (cap bytes with the NUL; out NULL skips it). Its whole length, which may reach
// cap (out then holds the start); (size_t)-1 when malformed.
static size_t js_string(Js *j, char *out, size_t cap) {
    size_t n = 0;
#define PUT(ch) do { if (out && n + 1 < cap) out[n] = (char)(ch); n++; } while (0)
    if (js_peek(j) != '"') return j->bad = 1, (size_t)-1;
    j->p++;
    for (;;) {
        if (j->p >= j->end) return j->bad = 1, (size_t)-1;
        unsigned c = (unsigned char)*j->p++;
        if (c == '"') break;
        if (c < 0x20) return j->bad = 1, (size_t)-1;
        if (c != '\\') {
            PUT(c);
            continue;
        }
        if (j->p >= j->end) return j->bad = 1, (size_t)-1;
        char e = *j->p++;
        if (e != 'u') {
            static const char from[] = "\"\\/bfnrt", to[] = "\"\\/\b\f\n\r\t";
            const char *k = e ? strchr(from, e) : NULL;
            if (!k) return j->bad = 1, (size_t)-1;
            PUT(to[k - from]);
            continue;
        }
        unsigned u;
        if (!js_hex4(j, &u)) return j->bad = 1, (size_t)-1;
        if (u >= 0xD800 && u < 0xDC00 && j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
            Js k = {j->p + 2, j->end, 0};
            unsigned lo;
            if (js_hex4(&k, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                j->p = k.p;
            }
        }
        static const unsigned lead[] = {0, 0xC0, 0xE0, 0xF0};
        int extra = u < 0x80 ? 0 : u < 0x800 ? 1 : u < 0x10000 ? 2 : 3;   // UTF-8 continuation bytes
        PUT(lead[extra] | u >> (6 * extra));
        for (int k = extra - 1; k >= 0; k--) PUT(0x80 | (u >> (6 * k) & 0x3F));
    }
    if (out && cap) out[n < cap ? n : cap - 1] = 0;
    return n;
#undef PUT
}

// After a member or an element: 1 when another follows (','), 0 at close (or malformed: bad).
static int js_next(Js *j, char close) {
    if (js_eat(j, ',')) return 1;
    if (!js_eat(j, close)) j->bad = 1;
    return 0;
}

// Skips the value at the cursor, at most 64 levels deep.
static void js_skip(Js *j, int depth) {
    int c = js_peek(j);
    if (c < 0 || depth > 64) {
        j->bad = 1;
        return;
    }
    if (c == '"') {
        js_string(j, NULL, 0);
        return;
    }
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        j->p++;
        if (js_eat(j, close)) return;
        do {
            if (c == '{' && (js_string(j, NULL, 0) == (size_t)-1 || !js_eat(j, ':'))) {
                j->bad = 1;
                return;
            }
            js_skip(j, depth + 1);
        } while (!j->bad && js_next(j, close));
        return;
    }
    const char *start = j->p;   // a number or true, false, null
    while (j->p < j->end && ((*j->p >= '0' && *j->p <= '9') || (*j->p >= 'a' && *j->p <= 'z') || *j->p == '-' ||
                             *j->p == '+' || *j->p == '.' || *j->p == 'E'))
        j->p++;
    if (j->p == start) j->bad = 1;
}

// An object member's key and its ':' (a key longer than cap reads as "", which matches nothing). 0 when malformed.
static int js_key(Js *j, char *key, size_t cap) {
    size_t n = js_string(j, key, cap);
    if (n == (size_t)-1 || !js_eat(j, ':')) return j->bad = 1, 0;
    if (n >= cap) key[0] = 0;
    return 1;
}

// A string value that fits out; 0 for null, another type or a longer string (out then "").
static int js_text(Js *j, char *out, size_t cap) {
    out[0] = 0;
    if (js_peek(j) != '"') return js_skip(j, 0), 0;
    size_t n = js_string(j, out, cap);
    if (n < cap) return 1;
    out[0] = 0;
    return 0;
}

static int js_true(Js *j) {
    int t = js_peek(j) == 't';
    js_skip(j, 0);
    return t;
}

// A whole number; 0 for another value (null, a string, a fraction) or more than 19 digits.
static uint64_t js_u64(Js *j) {
    int c = js_peek(j);
    if (c < '0' || c > '9') return js_skip(j, 0), 0;
    uint64_t v = 0;
    int digits = 0;
    for (; j->p < j->end && *j->p >= '0' && *j->p <= '9'; j->p++)
        if (++digits <= 19) v = v * 10 + (uint64_t)(*j->p - '0');
    int whole = digits <= 19;
    for (; j->p < j->end && *j->p && strchr(".eE+-0123456789", *j->p); j->p++) whole = 0;   // a fraction, an exponent
    return whole ? v : 0;
}

typedef struct {
    char version[32];     // the tag without its 'v': "0.3.13"
    char page[256];       // the release page; "" when the answer gave none of ours
    char url[512];        // the Windows setup's download URL
    char sha256[65];      // its SHA-256, 64 lower-case hex digits
    uint64_t size;
    int has_setup;        // url, sha256 and size are set
} Release;

typedef struct { char name[96], url[512], digest[80]; uint64_t size; } Asset;

static void js_assets(Js *j, Asset *a, int *n, int max) {
    if (!js_eat(j, '[')) {
        j->bad = 1;
        return;
    }
    if (js_eat(j, ']')) return;
    do {
        Asset x = {0};
        if (!js_eat(j, '{')) {
            j->bad = 1;
            return;
        }
        if (!js_eat(j, '}')) do {
            char key[32];
            if (!js_key(j, key, sizeof key)) return;
            if (!strcmp(key, "name")) js_text(j, x.name, sizeof x.name);
            else if (!strcmp(key, "browser_download_url")) js_text(j, x.url, sizeof x.url);
            else if (!strcmp(key, "digest")) js_text(j, x.digest, sizeof x.digest);   // null on assets before 2025
            else if (!strcmp(key, "size")) x.size = js_u64(j);
            else js_skip(j, 0);
        } while (!j->bad && js_next(j, '}'));
        if (j->bad) return;
        if (*n < max && x.name[0]) a[(*n)++] = x;
    } while (js_next(j, ']'));
}

// The release answer s (n bytes) into r; downloads must start with url_prefix. 0 with the reason in why when it is
// not a usable release; 1 also when it has no Windows setup (r->has_setup 0, the reason in why).
static int parse_release(const char *s, size_t n, const char *url_prefix, Release *r, char *why, size_t cap) {
    memset(r, 0, sizeof *r);
    *why = 0;
    Js j = {s, s + n, 0};
    char tag[64] = "", page[256] = "";
    int draft = 0, pre = 0, na = 0;
    Asset assets[16];
    if (!js_eat(&j, '{')) j.bad = 1;
    else if (!js_eat(&j, '}')) do {
        char key[32];
        if (!js_key(&j, key, sizeof key)) break;
        if (!strcmp(key, "tag_name")) js_text(&j, tag, sizeof tag);
        else if (!strcmp(key, "html_url")) js_text(&j, page, sizeof page);
        else if (!strcmp(key, "draft")) draft = js_true(&j);
        else if (!strcmp(key, "prerelease")) pre = js_true(&j);
        else if (!strcmp(key, "assets")) js_assets(&j, assets, &na, 16);
        else js_skip(&j, 0);
    } while (!j.bad && js_next(&j, '}'));
    if (j.bad) return snprintf(why, cap, "not the JSON of a release"), 0;
    unsigned v[3];
    int vpre;
    const char *ver = tag[0] == 'v' ? tag + 1 : tag;
    if (!parse_version(ver, v, &vpre) || vpre || strchr(ver, '+') || strlen(ver) >= sizeof r->version)
        return snprintf(why, cap, "the tag \"%s\" is not a release version", tag), 0;
    if (draft || pre) return snprintf(why, cap, "release %s is a draft or a pre-release", tag), 0;
    snprintf(r->version, sizeof r->version, "%s", ver);
    if (!strncmp(page, PAGE_PREFIX, strlen(PAGE_PREFIX))) snprintf(r->page, sizeof r->page, "%s", page);
    char want[96];
    snprintf(want, sizeof want, "TotK-Setup-%s-windows.zip", ver);
    for (int i = 0; i < na; i++) {
        const Asset *a = &assets[i];
        if (strcmp(a->name, want)) continue;
        const char *hex = !strncmp(a->digest, "sha256:", 7) ? a->digest + 7 : "";
        int hex_ok = strlen(hex) == 64;
        for (int k = 0; hex_ok && k < 64; k++)
            hex_ok = (hex[k] >= '0' && hex[k] <= '9') || (hex[k] >= 'a' && hex[k] <= 'f') || (hex[k] >= 'A' && hex[k] <= 'F');
        if (!hex_ok) return snprintf(why, cap, "%s has no SHA-256 digest", want), 1;
        if (strncmp(a->url, url_prefix, strlen(url_prefix)) || strchr(a->url + strlen(url_prefix), '?') ||
            strstr(a->url, ".."))
            return snprintf(why, cap, "%s is not on the project's download address: %s", want, a->url), 1;
        if (!a->size || a->size > SETUP_MAX) return snprintf(why, cap, "%s has a size of %llu bytes", want,
                                                             (unsigned long long)a->size), 1;
        snprintf(r->url, sizeof r->url, "%s", a->url);
        for (int k = 0; k < 64; k++) r->sha256[k] = (char)(hex[k] >= 'A' && hex[k] <= 'F' ? hex[k] + 32 : hex[k]);
        r->sha256[64] = 0;
        r->size = a->size;
        r->has_setup = 1;
        return 1;
    }
    snprintf(why, cap, "release %s has no %s", tag, want);
    return 1;
}

// ---- folders

// %LOCALAPPDATA%\TotK Recomp\updates (the setup keeps setup.json in the folder above, installer/recent.py); create:
// made when missing (a download), else 0 when missing (a check).
static int updates_dir(char *out, size_t cap, int create) {
    char base[PLAT_PATH_MAX];
    if (!plat_getenv("LOCALAPPDATA", base, sizeof base)) return 0;
    int n = snprintf(out, cap, "%s" SEP "TotK Recomp" SEP "updates", base);
    if (n < 0 || (size_t)n >= cap) return 0;
    if (create) plat_file_mkdir_p(out);
    return plat_dir_exists(out);
}

// The folder of totk.exe, without a trailing separator.
static int install_dir(char *out, size_t cap) {
    if (!plat_exe_path(out, cap)) return 0;
    char *cut = plat_last_sep(out);
    if (!cut) return 0;
    *cut = 0;
    return 1;
}

typedef struct { char **names; int n, cap; } Names;

static int collect(const char *name, int kind, uint64_t size, void *ctx) {
    (void)kind, (void)size;
    Names *l = ctx;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        char **grown = realloc(l->names, sizeof *grown * (size_t)cap);
        if (!grown) return 0;
        l->names = grown, l->cap = cap;
    }
    if ((l->names[l->n] = strdup(name)) != NULL) l->n++;
    return 1;
}

// Deletes path and everything under it; a link is deleted as a link, never followed. 1 when nothing is left.
static int remove_tree(const char *path) {
    int kind = plat_fs_kind(path);
    if (kind == PLAT_FS_MISSING) return 1;
    if (kind == PLAT_FS_FILE) return plat_fs_unlink(path) == PLAT_FS_OK;
    if (kind == PLAT_FS_LINK) return plat_fs_unlink(path) == PLAT_FS_OK || plat_fs_rmdir(path) == PLAT_FS_OK;
    Names l = {0};
    plat_fs_list(path, collect, &l);
    char *child = malloc(PLAT_PATH_MAX);
    for (int i = 0; i < l.n; i++) {
        if (child && snprintf(child, PLAT_PATH_MAX, "%s" SEP "%s", path, l.names[i]) < PLAT_PATH_MAX)
            remove_tree(child);
        free(l.names[i]);
    }
    free(child);
    free(l.names);
    return plat_fs_rmdir(path) == PLAT_FS_OK;
}

// An unpacked setup of version in dir: its VERSION names it (with or without the "+commit" a release adds), and the
// window's Python and code are there.
static int setup_folder_ok(const char *dir, const char *version) {
    char path[PLAT_PATH_MAX];
    snprintf(path, sizeof path, "%s" SEP "VERSION", dir);
    uint64_t size;
    char *text = (char *)plat_file_read(path, &size);
    size_t n = strlen(version);
    int ok = text && !strncmp(text, version, n) && strchr("+\r\n", text[n]);   // strchr finds the NUL too: an exact match
    free(text);
    snprintf(path, sizeof path, "%s" SEP "python" SEP "pythonw.exe", dir);
    ok = ok && plat_file_exists(path);
    snprintf(path, sizeof path, "%s" SEP "installer" SEP "gui.py", dir);
    return ok && plat_file_exists(path);
}

// The finished folder of version under updates\ (OK_MARK written after its checks) into dir. 0 when there is none.
static int setup_ready(const char *version, char *dir, size_t cap) {
    char base[PLAT_PATH_MAX], mark[PLAT_PATH_MAX];
    if (!updates_dir(base, sizeof base, 0)) return 0;
    snprintf(dir, cap, "%s" SEP "TotK-Setup-%s-windows", base, version);
    snprintf(mark, sizeof mark, "%s" SEP OK_MARK, dir);
    return plat_file_exists(mark) && setup_folder_ok(dir, version);
}

// ---- state shared with the menu

static PlatMutex g_lock = PLAT_MUTEX_INIT;   // g_st, g_rel, g_dir
static UpdateStatus g_st;
static Release g_rel;                 // the newer release the last check found
static char g_dir[PLAT_PATH_MAX];     // its unpacked setup in UPDATE_READY
static volatile int32_t g_busy;       // a check or download thread runs
static volatile int32_t g_cancel;     // update_cancel: the download stops at its next piece
static volatile int32_t g_opened;     // update_menu_opened ran
static int g_setup_made = -1;         // totk-install.json next to totk.exe; -1 not looked yet

static void publish(UpdateState s, UpdateError e) {
    plat_mutex_lock(&g_lock);
    g_st.state = s;
    g_st.error = e;
    plat_mutex_unlock(&g_lock);
}

// Scripted runs (--seconds, --boot, the menu screenshot test) neither check by themselves nor install.
static int scripted(void) {
    const RtConfig *c = rt_config();
    return (c && (c->seconds > 0 || c->boot)) || cfg_get("TOTK_MENU_TEST");
}

static int setup_made(void) {
    if (g_setup_made < 0) {
        char dir[PLAT_PATH_MAX], path[PLAT_PATH_MAX + 32];
        g_setup_made = install_dir(dir, sizeof dir) &&
                       snprintf(path, sizeof path, "%s" SEP "totk-install.json", dir) > 0 && plat_file_exists(path);
    }
    return g_setup_made;
}

// The releases/latest URL into url: GitHub's, or TOTK_UPDATE_API when it names a test server of this machine. prefix
// gets the start every download URL of the answer must have.
static void api_url(char *url, size_t cap, char *prefix, size_t pcap) {
    const char *v = cfg_get("TOTK_UPDATE_API");
    if (v && *v) {
        const char *rest = !strncmp(v, "http://127.0.0.1:", 17) || !strncmp(v, "http://localhost:", 17) ? v + 17 : NULL;
        const char *slash = rest ? strchr(rest, '/') : NULL;
        if (slash) {
            snprintf(url, cap, "%s", v);
            snprintf(prefix, pcap, "%.*s", (int)(slash + 1 - v), v);   // http://127.0.0.1:<port>/
            return;
        }
        rt_log("update: update_api \"%s\" is not http://127.0.0.1:<port>/... or http://localhost:<port>/...; asking "
               "GitHub", v);
    }
    snprintf(url, cap, "%s", API_URL);
    snprintf(prefix, pcap, "%s", DOWNLOAD_PREFIX);
}

typedef struct { char *buf; size_t n, cap; } Body;

static int body_piece(const void *data, size_t n, void *ctx) {
    Body *b = ctx;
    if (b->n + n + 1 > ANSWER_CAP) return 0;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        while (cap < b->n + n + 1) cap *= 2;
        char *grown = realloc(b->buf, cap);
        if (!grown) return 0;
        b->buf = grown, b->cap = cap;
    }
    memcpy(b->buf + b->n, data, n);
    b->n += n;
    b->buf[b->n] = 0;
    return 1;
}

// ---- the check
static void check_main(void *arg) {
    (void)arg;
    char url[512], prefix[256], err[256], why[256], dir[PLAT_PATH_MAX] = "";
    api_url(url, sizeof url, prefix, sizeof prefix);
    Body b = {0};
    Release r;
    UpdateState st = UPDATE_IDLE;
    UpdateError e = UPDATE_OK;
    int got = 0;
    if (!plat_http_get(url, API_HEADERS, body_piece, &b, NULL, err, sizeof err)) {
        rt_log("update: the check of %s failed: %s", url, err);
        e = UPDATE_ERR_CHECK;
    } else if (!parse_release(b.buf, b.n, prefix, &r, why, sizeof why)) {
        rt_log("update: the answer of %s is not usable: %s", url, why);
        e = UPDATE_ERR_CHECK;
    } else {
        got = 1;
        int newer = version_newer(r.version, rt_port_version);
        if (newer < 0) {
            rt_log("update: the latest release is %s; this build's version \"%s\" is not one to compare", r.version,
                   rt_port_version);
            e = UPDATE_ERR_VERSION;
        } else if (!newer) {
            rt_log("update: the latest release is %s; this build is %s", r.version, rt_port_version);
            st = UPDATE_CURRENT;
        } else if (!r.has_setup) {
            rt_log("update: release %s is newer than this build (%s), but %s", r.version, rt_port_version, why);
            st = UPDATE_NEWER, e = UPDATE_ERR_NO_SETUP;
        } else if (setup_ready(r.version, dir, sizeof dir)) {
            rt_log("update: release %s is newer than this build (%s); its setup is already in %s", r.version,
                   rt_port_version, dir);
            st = UPDATE_READY;
        } else {
            rt_log("update: release %s is newer than this build (%s): %s, %llu bytes", r.version, rt_port_version,
                   r.url, (unsigned long long)r.size);
            st = UPDATE_NEWER;
        }
    }
    free(b.buf);
    plat_mutex_lock(&g_lock);
    if (got) {
        g_rel = r;
        snprintf(g_st.version, sizeof g_st.version, "%s", r.version);
        snprintf(g_st.page, sizeof g_st.page, "%s", r.page);
        g_st.size = r.size;
        g_st.done = 0;
        snprintf(g_dir, sizeof g_dir, "%s", dir);
    }
    g_st.state = st;
    g_st.error = e;
    plat_mutex_unlock(&g_lock);
    plat_xchg32(&g_busy, 0);
}

// ---- the download, its checks and the unpacking

typedef struct {
    PlatFile *f;
    Sha256 sha;
    uint64_t done, size;
    int too_long, write_failed;
} Download;

static int download_piece(const void *data, size_t n, void *ctx) {
    Download *d = ctx;
    if (plat_cas32(&g_cancel, 0, 0)) return 0;
    if (d->done + n > d->size) return d->too_long = 1, 0;
    if (plat_file_pwrite(d->f, data, (uint32_t)n, d->done) != (int64_t)n) return d->write_failed = 1, 0;
    sha256_update(&d->sha, data, n);
    d->done += n;
    plat_mutex_lock(&g_lock);
    g_st.done = d->done;
    plat_mutex_unlock(&g_lock);
    return 1;
}

// Downloads, checks and unpacks r's setup into dir (updates\TotK-Setup-<version>-windows). UPDATE_OK, the failure,
// or -1 when cancelled. Nothing of a failed or cancelled attempt stays.
static int fetch_setup(const Release *r, char *dir, size_t cap) {
    char base[PLAT_PATH_MAX], work[PLAT_PATH_MAX], zip[PLAT_PATH_MAX], top[PLAT_PATH_MAX], name[64], err[256];
    if (!updates_dir(base, sizeof base, 1)) {
        rt_log("update: no folder for the download (LOCALAPPDATA)");
        return UPDATE_ERR_DISK;
    }
    snprintf(name, sizeof name, "TotK-Setup-%s-windows", r->version);
    snprintf(work, sizeof work, "%s" SEP "%s.part", base, name);
    snprintf(zip, sizeof zip, "%s" SEP "%s.zip", work, name);
    snprintf(top, sizeof top, "%s" SEP "%s", work, name);
    snprintf(dir, cap, "%s" SEP "%s", base, name);
    remove_tree(work);   // what an earlier run left
    plat_file_mkdir_p(work);
    int ferr = 0;
    PlatFile *f = plat_fs_create(zip, &ferr);
    if (!f) {
        rt_log("update: cannot create %s (error %d)", zip, plat_fs_os_error());
        remove_tree(work);
        return UPDATE_ERR_DISK;
    }
    Download d = {.f = f, .size = r->size};
    sha256_init(&d.sha);
    rt_log("update: downloading %s (%llu bytes) to %s", r->url, (unsigned long long)r->size, zip);
    uint64_t t0 = plat_now_ms();
    int ok = plat_http_get(r->url, NULL, download_piece, &d, NULL, err, sizeof err);
    plat_file_close(f);
    int e = UPDATE_OK;
    uint8_t sum[32];
    char hex[65];
    sha256_final(&d.sha, sum);
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", sum[i]);
    if (plat_cas32(&g_cancel, 0, 0)) {
        rt_log("update: download cancelled after %llu bytes", (unsigned long long)d.done);
        e = -1;
    } else if (d.write_failed) {
        rt_log("update: writing %s failed after %llu bytes (error %d)", zip, (unsigned long long)d.done,
               plat_fs_os_error());
        e = UPDATE_ERR_DISK;
    } else if (d.too_long) {
        rt_log("update: the download is longer than the %llu bytes the release lists", (unsigned long long)r->size);
        e = UPDATE_ERR_VERIFY;
    } else if (!ok) {
        rt_log("update: the download broke off after %llu bytes: %s", (unsigned long long)d.done, err);
        e = UPDATE_ERR_DOWNLOAD;
    } else if (d.done != r->size || strcmp(hex, r->sha256)) {
        rt_log("update: the download does not match the release: %llu bytes, SHA-256 %s; the release lists %llu "
               "bytes, SHA-256 %s", (unsigned long long)d.done, hex, (unsigned long long)r->size, r->sha256);
        e = UPDATE_ERR_VERIFY;
    }
    if (e) {
        remove_tree(work);
        return e;
    }
    rt_log("update: downloaded in %.1f s, size and SHA-256 match; unpacking", (plat_now_ms() - t0) / 1000.0);
    char zip_name[80];
    snprintf(zip_name, sizeof zip_name, "%s.zip", name);
    if (!plat_unzip(zip_name, work, UNZIP_MS, err, sizeof err)) {
        rt_log("update: unpacking %s failed: %s", zip, err);
        remove_tree(work);
        return UPDATE_ERR_DISK;
    }
    plat_fs_unlink(zip);
    if (!setup_folder_ok(top, r->version)) {
        rt_log("update: the zip does not hold the setup of %s in %s (VERSION, python, installer)", r->version, name);
        remove_tree(work);
        return UPDATE_ERR_VERIFY;
    }
    remove_tree(dir);   // an unfinished folder of an earlier attempt (no OK_MARK, else the check would have used it)
    char mark[PLAT_PATH_MAX];
    snprintf(mark, sizeof mark, "%s" SEP OK_MARK, dir);
    if (plat_fs_rename(top, dir, 1) != PLAT_FS_OK || !plat_file_write(mark, r->version, strlen(r->version))) {
        rt_log("update: cannot move the setup to %s (error %d)", dir, plat_fs_os_error());
        remove_tree(work);
        remove_tree(dir);
        return UPDATE_ERR_DISK;
    }
    remove_tree(work);
    rt_log("update: the setup of %s is ready in %s", r->version, dir);
    return UPDATE_OK;
}

static void download_main(void *arg) {
    (void)arg;
    Release r;
    char dir[PLAT_PATH_MAX];
    plat_mutex_lock(&g_lock);
    r = g_rel;
    plat_mutex_unlock(&g_lock);
    int e = fetch_setup(&r, dir, sizeof dir);
    plat_mutex_lock(&g_lock);
    if (e == UPDATE_OK) snprintf(g_dir, sizeof g_dir, "%s", dir);
    g_st.state = e == UPDATE_OK ? UPDATE_READY : UPDATE_NEWER;
    g_st.error = e > 0 ? (UpdateError)e : UPDATE_OK;
    g_st.done = 0;
    plat_mutex_unlock(&g_lock);
    plat_xchg32(&g_cancel, 0);
    plat_xchg32(&g_busy, 0);
}

// ---- the menu's calls

int update_supported(void) {
#ifdef _WIN32
    return 1;
#else
    return 0;
#endif
}

void update_status(UpdateStatus *out) {
    plat_mutex_lock(&g_lock);
    *out = g_st;
    plat_mutex_unlock(&g_lock);
    out->cancelling = out->state == UPDATE_DOWNLOADING && plat_cas32(&g_cancel, 0, 0);
    out->setup_made = setup_made();
}

void update_check(void) {
    if (!update_supported() || plat_cas32(&g_busy, 1, 0)) return;
    publish(UPDATE_CHECKING, UPDATE_OK);
    if (!plat_thread_spawn(check_main, NULL, "update", 0)) {
        rt_log("update: cannot start the check thread");
        publish(UPDATE_IDLE, UPDATE_ERR_CHECK);
        plat_xchg32(&g_busy, 0);
    }
}

void update_menu_opened(void) {
    if (g_opened || plat_xchg32(&g_opened, 1)) return;
    if (!update_supported()) return;
    if (!cfg_bool("TOTK_UPDATE_CHECK")) rt_log("update: update_check = 0; no check");
    else if (scripted()) rt_log("update: a scripted run; no check");
    else update_check();
}

void update_download(void) {
    plat_mutex_lock(&g_lock);
    int ok = g_st.state == UPDATE_NEWER && g_rel.has_setup;
    plat_mutex_unlock(&g_lock);
    if (!ok || plat_cas32(&g_busy, 1, 0)) return;
    plat_mutex_lock(&g_lock);
    g_st.state = UPDATE_DOWNLOADING;
    g_st.error = UPDATE_OK;
    g_st.done = 0;
    plat_mutex_unlock(&g_lock);
    plat_xchg32(&g_cancel, 0);
    if (!plat_thread_spawn(download_main, NULL, "update", 0)) {
        rt_log("update: cannot start the download thread");
        publish(UPDATE_NEWER, UPDATE_ERR_DOWNLOAD);
        plat_xchg32(&g_busy, 0);
    }
}

void update_cancel(void) { plat_xchg32(&g_cancel, 1); }

int update_install(void) {
    char dir[PLAT_PATH_MAX], python[PLAT_PATH_MAX + 32], install[PLAT_PATH_MAX];
    plat_mutex_lock(&g_lock);
    int ready = g_st.state == UPDATE_READY;
    snprintf(dir, sizeof dir, "%s", g_dir);
    plat_mutex_unlock(&g_lock);
    if (!ready || !setup_made() || scripted() || !install_dir(install, sizeof install)) return 0;
    snprintf(python, sizeof python, "%s" SEP "python" SEP "pythonw.exe", dir);
    // the setup's window code is in the release folder, on the embedded Python's path through its ._pth file
    const char *argv[] = {python, "-m", "installer.gui", "--upgrade", install, NULL};
    rt_log("update: starting the setup in %s on %s; the game closes", dir, install);
    if (!plat_spawn(argv, dir)) {
        rt_log("update: the setup did not start");
        publish(UPDATE_READY, UPDATE_ERR_START);
        return 0;
    }
    return 1;
}

// ---- --probe-update: the version comparison and the release answer; with TOTK_UPDATE_API, a check and a download
// against that test server (LOCALAPPDATA chooses where the download goes). Prints "update: probe state=<n> error=<n>"
// for tests/update_test.py. No game files.

static const char probe_answer[] =
    "{\"url\":\"https://api.github.com/repos/" REPO "/releases/1\",\"html_url\":\"https://github.com/" REPO
    "/releases/tag/v0.3.13\",\"id\":1,\"author\":{\"login\":\"someone\",\"site_admin\":false},\"tag_name\":\"v0.3.13\","
    "\"draft\":false,\"prerelease\":false,\"name\":\"v0.3.13 \\u2014 \\\"notes\\\" \\ud83d\\ude00\",\"assets\":["
    "{\"name\":\"TotK-Setup-x86_64-0.3.13-linux.AppImage\",\"size\":229976568,\"digest\":\"sha256:"
    "b0c992f1e879401c119c796badc43687c3a5ac804d0d8c17f37fb16925f0e3ad\",\"browser_download_url\":\"https://github.com/"
    REPO "/releases/download/v0.3.13/TotK-Setup-x86_64-0.3.13-linux.AppImage\",\"uploader\":{\"id\":2}},"
    "{\"name\":\"TotK-Setup-0.3.13-windows.zip\",\"label\":null,\"size\":164418306,\"digest\":\"sha256:"
    "62D3936500B8B336287E9D9C2E329D92044C62490D889997FE26EC1F616A5B5E\",\"browser_download_url\":\"https://github.com/"
    REPO "/releases/download/v0.3.13/TotK-Setup-0.3.13-windows.zip\",\"download_count\":12,\"uploader\":{\"id\":2}}],"
    "\"body\":\"## Changes\\r\\n- one\\n- two [x](y)\",\"reactions\":{\"+1\":3}}";

int probe_update(void) {
    int fails = 0;
#define CHECK(c, what) do { if (!(c)) { fails++; fprintf(stderr, "FAIL update: %s\n", what); } } while (0)
    CHECK(version_newer("0.3.13", "0.3.12+eda417f") == 1, "0.3.13 is newer than 0.3.12+eda417f");
    CHECK(version_newer("0.3.12", "0.3.12+eda417f") == 0, "0.3.12 is not newer than 0.3.12+eda417f");
    CHECK(version_newer("0.3.12", "0.3.13") == 0, "0.3.12 is not newer than 0.3.13");
    CHECK(version_newer("0.3.13", "0.3.13-dev+1a2b3c4") == 1, "a release is newer than its pre-release");
    CHECK(version_newer("0.10.0", "0.9.9") == 1, "versions compare as numbers");
    CHECK(version_newer("1.0.0", "0.99.99") == 1, "the major number first");
    CHECK(version_newer("0.3.14-rc1", "0.3.13") == -1, "a pre-release tag is not a release");
    CHECK(version_newer("0.3.14", "unknown") == -1, "an unknown running version");
    CHECK(version_newer("0.3", "0.2.0") == -1 && version_newer("0.3.1x", "0.2.0") == -1, "not three numbers");

    Release r;
    char why[256];
    CHECK(parse_release(probe_answer, sizeof probe_answer - 1, DOWNLOAD_PREFIX, &r, why, sizeof why) &&
              r.has_setup && !strcmp(r.version, "0.3.13") && r.size == 164418306 &&
              !strcmp(r.sha256, "62d3936500b8b336287e9d9c2e329d92044c62490d889997fe26ec1f616a5b5e") &&
              !strcmp(r.url, DOWNLOAD_PREFIX "v0.3.13/TotK-Setup-0.3.13-windows.zip") &&
              !strcmp(r.page, PAGE_PREFIX "tag/v0.3.13"),
          "GitHub's release answer gives the Windows setup");
    CHECK(parse_release(probe_answer, sizeof probe_answer - 1, "http://127.0.0.1:1/", &r, why, sizeof why) &&
              !r.has_setup, "a download URL elsewhere than the prefix is refused");
    static const char *const bad[] = {
        "", "[]", "{\"tag_name\":\"v0.3.13\"", "{\"tag_name\":\"v0.3.13\",}", "{\"tag_name\":\"0.3.13-rc1\"}",
        "{\"tag_name\":\"v0.3.13\",\"draft\":true}", "{\"tag_name\":\"v0.3.13\",\"prerelease\":true}",
        "{\"tag_name\":\"v0.3.13\",\"assets\":[{\"name\":\"x\"}],\"x\":\"\\q\"}", "{\"tag_name\":\"nightly\"}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        char what[96];
        snprintf(what, sizeof what, "malformed or unusable answer %zu is refused", i);
        CHECK(!parse_release(bad[i], strlen(bad[i]), DOWNLOAD_PREFIX, &r, why, sizeof why), what);
    }
    static const char no_digest[] = "{\"tag_name\":\"v0.4.0\",\"assets\":[{\"name\":\"TotK-Setup-0.4.0-windows.zip\","
                                    "\"size\":5,\"digest\":null,\"browser_download_url\":\"" DOWNLOAD_PREFIX
                                    "v0.4.0/TotK-Setup-0.4.0-windows.zip\"}]}";
    CHECK(parse_release(no_digest, sizeof no_digest - 1, DOWNLOAD_PREFIX, &r, why, sizeof why) && !r.has_setup,
          "a setup without a digest is not offered");

    const char *api = cfg_get("TOTK_UPDATE_API");
    if (api && *api) {
        // the module's own jobs against the test server, each waited for
        update_check();
        for (int i = 0; i < 600 && plat_cas32(&g_busy, 0, 0); i++) plat_sleep_ms(100);
        UpdateStatus s;
        update_status(&s);
        if (s.state == UPDATE_NEWER && !s.error) {
            update_download();
            for (int i = 0; i < 6000 && plat_cas32(&g_busy, 0, 0); i++) plat_sleep_ms(100);
            update_status(&s);
        }
        fprintf(stderr, "update: probe state=%d error=%d version=%s dir=%s\n", (int)s.state, (int)s.error, s.version,
                g_dir);
    }
    fprintf(stderr, "update: probe %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
#undef CHECK
}
