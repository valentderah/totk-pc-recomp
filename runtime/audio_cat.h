// Sound categories for the music / effects / voice volume settings (audio_cat.c).
#ifndef TOTK_AUDIO_CAT_H
#define TOTK_AUDIO_CAT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {
    AUDIO_CAT_UNKNOWN = 0,
    AUDIO_CAT_MUSIC   = 1,
    AUDIO_CAT_SFX     = 2,
    AUDIO_CAT_VOICE   = 3,
};

int audio_cat_active(void);                    // one of volume_music / volume_sfx / volume_voice is below 100
int audio_cat_from_path(const char *path);     // a RomFS-relative path; UNKNOWN outside Sound/Resource/
void audio_cat_tag_range(uint64_t addr, uint64_t size, int cat);
void audio_cat_scan_bars(uint64_t dst, uint64_t size, int fallback_cat);
void audio_cat_scan_bwav(const void *data, uint64_t size, int cat);
void audio_cat_on_opus_decode(uint64_t decoder_guest, uint64_t in_addr, uint64_t out_addr, uint64_t out_size);
int audio_cat_lookup(uint64_t addr);
int audio_cat_lookup_coef(uint64_t extra_addr, uint64_t extra_size);
void audio_cat_log_tables(void);

#ifdef __cplusplus
}
#endif
#endif
