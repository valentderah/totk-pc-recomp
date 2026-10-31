#include "prompt_atlas.h"
#include "input_prompts.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

extern void dm_guest_write_prepare(void *, size_t);
static void (*update_cache)(void *);
static void (*update_manager)(void *);
static void *(*map_buffer)(void *);
static pthread_mutex_t atlas_lock = PTHREAD_MUTEX_INITIALIZER;

// The original font's ink bounds preserve each layout's advance, baseline and
// animation. Only the pixels inside that rectangle change. No game font is
// distributed: the bounds are measured from the player's live cache.
typedef struct {
    uint32_t code;
    uint16_t font, width, height, x, y, ink_width, ink_height;
    NativePromptGlyph glyph;
    uint8_t *pixels;
} GlyphTile;
static GlyphTile tiles[512];
static unsigned tile_count;

static uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v,p,2); return v; }
static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v,p,4); return v; }
static uint64_t u64(const uint8_t *p) { uint64_t v; memcpy(&v,p,8); return v; }
static uint8_t *pointer(const uint8_t *p) { return (uint8_t *)(uintptr_t)u64(p); }

static GlyphTile *tile_for(uint32_t code, unsigned font, unsigned w, unsigned h,
                           const uint8_t *atlas, unsigned stride, unsigned ah, unsigned x, unsigned y) {
    for (unsigned i=0;i<tile_count;i++) {
        GlyphTile *t=&tiles[i];
        if(t->code==code && t->font==font && t->width==w && t->height==h)return t;
    }
    // Keeping the bounds is necessary: measuring a replacement would gradually
    // shrink it on each device change. An exhausted cache leaves the glyph as is.
    if(tile_count==sizeof tiles/sizeof *tiles)return NULL;
    unsigned left=w,top=h,right=0,bottom=0;
    for(unsigned row=0;row<h;row++)for(unsigned col=0;col<w;col++) {
        if(!atlas[(size_t)(ah-1-y-row)*stride+x+1+col])continue;
        if(col<left)left=col;if(row<top)top=row;
        if(col+1>right)right=col+1;if(row+1>bottom)bottom=row+1;
    }
    if(right<=left || bottom<=top)return NULL;
    uint8_t *pixels=calloc((size_t)w*h,2);
    if(!pixels)return NULL;
    GlyphTile *t=&tiles[tile_count++];
    *t=(GlyphTile){.code=code,.font=font,.width=w,.height=h,
        .x=left,.y=top,.ink_width=right-left,.ink_height=bottom-top,.pixels=pixels};
    return t;
}
static void replace_glyph(uint8_t *cache, uint8_t *entry, uint8_t *atlas,
                          unsigned aw, unsigned ah, const NativePromptSnapshot *snapshot) {
    uint32_t code=u32(entry+0x30);
    NativePromptGlyph glyph;
    if(!native_prompt_glyph(code,snapshot,&glyph))return;
    unsigned font=u16(entry+0x34);
    const char *names=(const char *)pointer(cache+0x18);
    // Other private-use fonts contain story symbols with unrelated meanings.
    if(!names)return;
    if(memcmp(names+(size_t)font*50,"External\0",9))return;
    unsigned x=u16(entry+0x40),y=u16(entry+0x42),w=u16(entry+0x44),h=u16(entry+0x46);
    if(!w || !h || w>512 || h>512 || x+1+w>aw || y+h>ah)return;
    GlyphTile *tile=tile_for(code,font,w,h,atlas,aw,ah,x,y);
    if(!tile)return;
    if(!tile->glyph.label || tile->glyph.kind!=glyph.kind || tile->glyph.detail!=glyph.detail || strcmp(tile->glyph.label,glyph.label)) {
        uint8_t *ink=tile->pixels+(size_t)w*h;
        if(!native_prompt_raster(&glyph,ink,tile->ink_width,tile->ink_height))return;
        memset(tile->pixels,0,(size_t)w*h);
        for(unsigned row=0;row<tile->ink_height;row++)
            memcpy(tile->pixels+(size_t)(row+tile->y)*w+tile->x,ink+(size_t)row*tile->ink_width,tile->ink_width);
        tile->glyph=glyph;
    }
    for(unsigned row=0;row<h;row++) {
        uint8_t *dest=atlas+(size_t)(ah-1-y-row)*aw+x+1;
        const uint8_t *src=tile->pixels+(size_t)row*w;
        if(!memcmp(dest,src,w))continue;
        dm_guest_write_prepare(dest,w);
        memcpy(dest,src,w);
    }
}
typedef struct {uint8_t *pixels;unsigned width,height,rows;} Atlas;
static bool atlas_open(uint8_t *cache,Atlas *out) {
    unsigned width=u32(cache+0x28),height=u32(cache+0x2c),rows=u32(cache+0x1880);
    if(!width || !height || width>8192 || height>8192 || rows>256)return false;
    uint8_t *buffer=pointer(cache+0x1e98);
    if(!buffer)return false;
    uint8_t *pixels=map_buffer(buffer);
    if(!pixels)return false;
    *out=(Atlas){pixels+u64(cache+0x1ea0),width,height,rows};
    return true;
}
static bool resident(uint8_t *cache,uint8_t *entry,unsigned rows) {
    unsigned row=entry[0x3b];
    if(row>=rows)return false;
    uint8_t *head=cache+0x80+row*24,*node=pointer(head+8);
    for(unsigned n=0;node && node!=head && n<8192;n++,node=pointer(node+8))
        if(node==entry+0x10)return true;
    return false;
}
// Called with atlas_lock held, after the game has finished writing this cache.
static uint64_t refresh(uint8_t *cache) {
    NativePromptSnapshot snapshot=native_prompts_snapshot();Atlas atlas;
    if(!atlas_open(cache,&atlas))return 0;
    for(unsigned i=0;i<atlas.rows;i++) {
        uint8_t *head=cache+0x80+i*24,*node=pointer(head+8);
        unsigned count=0;
        while(node && node!=head && count++<8192) {
            replace_glyph(cache,node-0x10,atlas.pixels,atlas.width,atlas.height,&snapshot);
            node=pointer(node+8);
        }
    }
    return snapshot.revision;
}
void native_prompt_cache_update(void *owner) {
    uint8_t *cache=owner,*head=cache+0x50,*node=pointer(head+8);
    uint8_t *pending[512];unsigned count=0,walked=0;bool overflow=false;
    while(node && node!=head && walked++<32768) {
        unsigned code=u32(node+0x30);
        if(code>=0xe040 && code<=0xe068) {
            if(count<sizeof pending/sizeof *pending)pending[count++]=node;
            else overflow=true;
        }
        node=pointer(node+8);
    }
    update_cache(owner);
    // Even when no prompt was added, this unlock publishes the completed font
    // worker's writes to the UI owner before it examines the cache metadata.
    pthread_mutex_lock(&atlas_lock);
    if(overflow)refresh(cache);
    else if(count) {
        NativePromptSnapshot snapshot=native_prompts_snapshot();Atlas atlas;
        if(atlas_open(cache,&atlas))for(unsigned i=0;i<count;i++) {
            uint8_t *entry=pending[i];
            if(entry[0x38]&0x40)continue; // the font did not contain this glyph
            // Allocation failures retain the entry's old rectangle. Only a
            // successful insertion into its atlas row owns those pixels.
            if(!resident(cache,entry,atlas.rows))continue;
            replace_glyph(cache,entry,atlas.pixels,atlas.width,atlas.height,&snapshot);
        }
    }
    pthread_mutex_unlock(&atlas_lock);
}
void native_prompt_manager_update(void *owner) {
    update_manager(owner);
    uint8_t *manager=owner;
    // The UI owner's job-state flags prevent a refresh while it has queued a
    // font job. The worker's mutex publication covers the cache reads below.
    if(manager[0x3d])return;
    pthread_mutex_lock(&atlas_lock);
    uint8_t *cache=pointer(manager+0x10),*worker=pointer(manager+0x28);
    if(!cache || !worker || worker[0xe0]){pthread_mutex_unlock(&atlas_lock);return;}
    typedef struct {void *manager,*cache;uint64_t revision;} Seen;
    static _Thread_local Seen seen[8];
    unsigned slot=0;
    while(slot<7 && seen[slot].manager && seen[slot].manager!=owner)slot++;
    uint64_t revision=native_prompts_revision();
    if(seen[slot].manager!=owner || seen[slot].cache!=cache || seen[slot].revision!=revision)
        seen[slot]=(Seen){owner,cache,refresh(cache)};
    pthread_mutex_unlock(&atlas_lock);
}
void native_prompt_atlas_bind(void *(*buffer_map)(void *), void (*cache_update)(void *), void (*manager_update)(void *)) {
    map_buffer=buffer_map;update_cache=cache_update;update_manager=manager_update;
}
