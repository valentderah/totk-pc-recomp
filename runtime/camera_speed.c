// Camera speed: TOTK_STICK_SENS (0.1 to 3, default 1, read at start) multiplies the game's own right-stick camera speeds.
// The game's Options menu offers five camera sensitivities, Very Low to Very High; their turn rates are the array
// StickSensitivity {0.8, 1.2, 1.6, 2.0, 2.4} in Component/PlayerCameraParam/PlayerCamera.game__component__
// PlayerCameraParam.bgyml inside Pack/Actor/PlayerCamera.pack.zs. romfs.c serves a copy of that pack with every entry
// multiplied (cam_pack_build), so the setting raises the camera's top speed as well as its speed at a partial tilt, at
// any of the five sensitivities. The mouse camera reaches the game as a right-stick deflection (input.c, mouse_camera),
// which divides it by the same factor (cam_speed_applied), so TOTK_MOUSE_SENS alone sets the mouse's speed.
//
// The pack is a SARC archive compressed with the dictionary pack.zsdic of Pack/ZsDic.pack.zs. A float in a BYML array
// is stored in the array itself, so the multiplied values take the bytes of the old ones and no offset moves.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "cfg.h"
#include "host.h"

#define MEMBER "Component/PlayerCameraParam/PlayerCamera.game__component__PlayerCameraParam.bgyml"
#define MAX_RAW (64u << 20)

static float applied = 1.0f;

float cam_speed_wanted(void) {
    double v = cfg_double("TOTK_STICK_SENS");
    return (float)(v < 0.1 ? 0.1 : v > 3 ? 3 : v);
}

float cam_speed_applied(void) { return applied; }

static int u32_at(const uint8_t *p, size_t n, size_t off, uint32_t *v) {
    if (off > n || n - off < 4) return 0;
    memcpy(v, p + off, 4);
    return 1;
}

// SARC member `name` of the archive p[0..n): its start and size; 0 when it is not there.
static int sarc_member(const uint8_t *p, size_t n, const char *name, size_t *at, size_t *size) {
    uint32_t data;
    uint16_t head, count, bom;
    if (n < 0x14 || memcmp(p, "SARC", 4)) return 0;
    memcpy(&head, p + 4, 2);
    memcpy(&bom, p + 6, 2);
    if (bom != 0xfeff || !u32_at(p, n, 12, &data) || head + 12u > n || memcmp(p + head, "SFAT", 4)) return 0;
    memcpy(&count, p + head + 6, 2);
    size_t names = head + 12 + (size_t)count * 16;
    if (names + 8 > n || memcmp(p + names, "SFNT", 4)) return 0;
    names += 8;
    for (unsigned i = 0; i < count; i++) {
        uint32_t attr, lo, hi;
        size_t e = head + 12 + (size_t)i * 16;
        if (!u32_at(p, n, e + 4, &attr) || !u32_at(p, n, e + 8, &lo) || !u32_at(p, n, e + 12, &hi)) return 0;
        size_t s = names + (size_t)(attr & 0xffffff) * 4, len = strlen(name);
        if (!(attr >> 24) || s + len + 1 > n || memcmp(p + s, name, len + 1)) continue;
        if (lo > hi || data + (size_t)hi > n) return 0;
        *at = data + lo;
        *size = hi - lo;
        return 1;
    }
    return 0;
}

// The StickSensitivity floats of the BYML p[0..n), multiplied by k; their count, 0 when the file has no such array.
static int scale_sensitivity(uint8_t *p, size_t n, float k, float was[8]) {
    uint32_t keys, root, head;
    if (n < 16 || memcmp(p, "YB", 2) || !u32_at(p, n, 4, &keys) || !u32_at(p, n, 12, &root) ||
        !u32_at(p, n, root, &head) || (head & 0xff) != 0xc1 || !u32_at(p, n, keys, &head) || (head & 0xff) != 0xc2)
        return 0;
    uint32_t nkeys = head >> 8;
    if (!u32_at(p, n, root, &head)) return 0;
    for (uint32_t i = 0; i < head >> 8; i++) {
        uint32_t e, at, arr, ah;
        if (!u32_at(p, n, root + 4 + 8 * (size_t)i, &e) || (e & 0xffffff) >= nkeys ||
            !u32_at(p, n, keys + 4 + 4 * (size_t)(e & 0xffffff), &at))
            return 0;
        const char *name = (const char *)p + keys + at;
        if (keys + at + sizeof "StickSensitivity" > n || strcmp(name, "StickSensitivity")) continue;
        if (e >> 24 != 0xc0 || !u32_at(p, n, root + 8 + 8 * (size_t)i, &arr) || !u32_at(p, n, arr, &ah) ||
            (ah & 0xff) != 0xc0)
            return 0;
        uint32_t count = ah >> 8;
        size_t vals = arr + 4 + ((count + 3) & ~3u);
        if (!count || count > 8 || vals + 4 * (size_t)count > n) return 0;
        for (uint32_t j = 0; j < count; j++)
            if (p[arr + 4 + j] != 0xd2) return 0;
        for (uint32_t j = 0; j < count; j++) {
            float v;
            memcpy(&v, p + vals + 4 * j, 4);
            was[j] = v;
            v *= k;
            memcpy(p + vals + 4 * j, &v, 4);
        }
        return (int)count;
    }
    return 0;
}

int cam_pack_build(const void *in, size_t n, const void *zsdic, size_t zsdic_n, void **out, size_t *out_n) {
    float k = cam_speed_wanted();
    *out = NULL;
    if (k == 1.0f) return 0;
    int ok = 0;
    uint8_t *dics = NULL, *raw = NULL;
    ZSTD_DDict *dd = NULL;
    ZSTD_CDict *cd = NULL;
    void *z = NULL;
    // The dictionaries: Pack/ZsDic.pack.zs is a SARC compressed without one.
    unsigned long long dn = ZSTD_getFrameContentSize(zsdic, zsdic_n);
    size_t dic_at, dic_n;
    if (dn == ZSTD_CONTENTSIZE_UNKNOWN || dn == ZSTD_CONTENTSIZE_ERROR || dn > MAX_RAW || !(dics = malloc((size_t)dn)) ||
        ZSTD_decompress(dics, (size_t)dn, zsdic, zsdic_n) != dn ||
        !sarc_member(dics, (size_t)dn, "pack.zsdic", &dic_at, &dic_n)) {
        rt_log("camera speed: no pack.zsdic in Pack/ZsDic.pack.zs; the camera keeps its own speed");
        goto done;
    }
    unsigned long long rn = ZSTD_getFrameContentSize(in, n);
    unsigned id = ZSTD_getDictID_fromFrame(in, n);
    dd = ZSTD_createDDict(dics + dic_at, dic_n);
    if (!dd || id != ZSTD_getDictID_fromDDict(dd) || rn == ZSTD_CONTENTSIZE_UNKNOWN ||
        rn == ZSTD_CONTENTSIZE_ERROR || rn > MAX_RAW || !(raw = malloc((size_t)rn))) {
        rt_log("camera speed: Pack/Actor/PlayerCamera.pack.zs is not a pack.zsdic frame with a size; the camera keeps its "
               "own speed");
        goto done;
    }
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    size_t got = dctx ? ZSTD_decompress_usingDDict(dctx, raw, (size_t)rn, in, n, dd) : 0;
    ZSTD_freeDCtx(dctx);
    size_t at, size;
    float was[8];
    int count = !ZSTD_isError(got) && got == rn && sarc_member(raw, (size_t)rn, MEMBER, &at, &size)
                    ? scale_sensitivity(raw + at, size, k, was) : 0;
    if (!count) {
        rt_log("camera speed: no StickSensitivity array in %s; the camera keeps its own speed", MEMBER);
        goto done;
    }
    cd = ZSTD_createCDict(dics + dic_at, dic_n, 9);
    size_t cap = ZSTD_compressBound((size_t)rn), zn = 0;
    ZSTD_CCtx *cctx = cd ? ZSTD_createCCtx() : NULL;
    if (cctx && (z = malloc(cap))) zn = ZSTD_compress_usingCDict(cctx, z, cap, raw, (size_t)rn, cd);
    ZSTD_freeCCtx(cctx);
    if (!z || !zn || ZSTD_isError(zn)) goto done;
    *out = z;
    *out_n = zn;
    z = NULL;
    ok = 1;
    rt_log("camera speed: x%.2f, StickSensitivity %.2f %.2f %.2f %.2f %.2f -> %.2f %.2f %.2f %.2f %.2f", k, was[0],
           count > 1 ? was[1] : 0, count > 2 ? was[2] : 0, count > 3 ? was[3] : 0, count > 4 ? was[4] : 0, was[0] * k,
           count > 1 ? was[1] * k : 0, count > 2 ? was[2] * k : 0, count > 3 ? was[3] * k : 0,
           count > 4 ? was[4] * k : 0);
done:
    free(z);
    free(raw);
    free(dics);
    ZSTD_freeDDict(dd);
    ZSTD_freeCDict(cd);
    return ok;
}

void cam_pack_served(void) { applied = cam_speed_wanted(); }
