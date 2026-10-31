#include "input_prompts.h"
#include "input_bindings.h"
#include "prompt_atlas.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned writes;
void dm_guest_write_prepare(void *p,size_t n){(void)p;(void)n;writes++;}
static void *map(void *buffer){return buffer;}
static void no_update(void *owner){(void)owner;}
static void put16(uint8_t *p,unsigned value){uint16_t v=value;memcpy(p,&v,2);}
static void put32(uint8_t *p,unsigned value){uint32_t v=value;memcpy(p,&v,4);}
static void putptr(uint8_t *p,void *value){memcpy(p,&value,sizeof value);}
static uint8_t *pending_entry;
static unsigned pending_failure;
static void finish_pending(void *owner){
    uint8_t *cache=owner,*entry=pending_entry;
    putptr(cache+0x58,cache+0x50);
    if(pending_failure){
        // Both atlas-full and oversized-glyph errors retain stale coordinates.
        putptr(cache+0x68,entry);putptr(entry+8,cache+0x60);
        put32(cache+0x1e90,pending_failure);
    }else{
        putptr(cache+0x88,entry+0x10);putptr(entry+0x18,cache+0x80);
    }
}
static void expect(unsigned code,NativePromptKind kind,const char *label){
    NativePromptSnapshot s=native_prompts_snapshot();NativePromptGlyph glyph;
    assert(native_prompt_glyph(code,&s,&glyph));assert(glyph.kind==kind);assert(!strcmp(glyph.label,label));
}
int main(void){
    assert(native_key_button(14)==1);assert(native_key_button(36)==1);
    assert(native_key_button(49)==4);assert(native_key_button(56)==2);assert(native_key_button(60)==2);
    assert(native_key_button(48)==1024);assert(native_key_button(46)==2048);
    assert(native_key_button(59)==256);assert(native_key_button(62)==256);
    assert(native_key_button(15)==512);assert(native_key_button(3)==128);
    assert(native_key_button(38)==0);assert(native_key_button(40)==0);
    assert(native_mouse_button_mapping(0)==8);assert(native_mouse_button_mapping(1)==256);
    assert(native_mouse_button_mapping(999)==0);
    assert(native_prompts_identify("DualSense Wireless Controller",NULL)==NATIVE_PROMPT_PLAYSTATION);
    assert(native_prompts_identify(NULL,"Xbox Wireless Controller")==NATIVE_PROMPT_XBOX);
    assert(native_prompts_identify("Switch Pro Controller",NULL)==NATIVE_PROMPT_SWITCH);
    assert(native_prompts_identify(NULL,NULL)==NATIVE_PROMPT_GENERIC);
    native_prompts_use(NATIVE_PROMPT_KEYBOARD,false);
    expect(0xe040,NATIVE_GLYPH_KEY,"E");expect(0xe042,NATIVE_GLYPH_KEY,"Space");
    expect(0xe043,NATIVE_GLYPH_MOUSE,"");expect(0xe047,NATIVE_GLYPH_KEY,"R");
    expect(0xe056,NATIVE_GLYPH_KEY,"C");expect(0xe04b,NATIVE_GLYPH_KEY,"Tab");
    native_prompts_use(NATIVE_PROMPT_XBOX,false);
    expect(0xe040,NATIVE_GLYPH_CIRCLE,"B");expect(0xe041,NATIVE_GLYPH_CIRCLE,"A");
    expect(0xe042,NATIVE_GLYPH_CIRCLE,"Y");expect(0xe043,NATIVE_GLYPH_CIRCLE,"X");
    expect(0xe044,NATIVE_GLYPH_SHOULDER,"LB");expect(0xe047,NATIVE_GLYPH_SHOULDER,"RT");
    native_prompts_use(NATIVE_PROMPT_PLAYSTATION,false);
    expect(0xe040,NATIVE_GLYPH_CIRCLE,"");expect(0xe041,NATIVE_GLYPH_CROSS,"");
    expect(0xe042,NATIVE_GLYPH_TRIANGLE,"");expect(0xe043,NATIVE_GLYPH_SQUARE,"");
    native_prompts_use(NATIVE_PROMPT_SWITCH,false);
    expect(0xe040,NATIVE_GLYPH_CIRCLE,"A");expect(0xe041,NATIVE_GLYPH_CIRCLE,"B");
    native_prompts_use(NATIVE_PROMPT_XBOX,true);expect(0xe040,NATIVE_GLYPH_CIRCLE,"A");
    NativePromptSnapshot remap=native_prompts_snapshot();remap.buttons[0]=1;remap.buttons[1]=2;
    native_prompts_controller_map(remap.buttons);native_prompts_use(NATIVE_PROMPT_XBOX,false);
    expect(0xe040,NATIVE_GLYPH_CIRCLE,"A");expect(0xe041,NATIVE_GLYPH_CIRCLE,"B");
    uint16_t defaults[17]={2,1,8,4,16,32,64,128,256,512,1024,2048,4096,8192,16384,32768,2048};
    native_prompts_controller_map(defaults);
    uint64_t revision=native_prompts_revision();native_prompts_use(NATIVE_PROMPT_XBOX,false);
    assert(native_prompts_revision()==revision);
    for(unsigned device=0;device<=NATIVE_PROMPT_GENERIC;device++) {
        native_prompts_use(device,false);NativePromptSnapshot s=native_prompts_snapshot();
        uint8_t diagram[580*340*4];assert(native_prompt_diagram(&s,diagram,580,340));
        unsigned visible=0;for(unsigned i=3;i<sizeof diagram;i+=4)visible+=diagram[i]!=0;
        assert(visible>20000 && visible<580*340);
        for(unsigned code=0xe040;code<=0xe092;code++) {
            NativePromptGlyph g;if(!native_prompt_glyph(code,&s,&g))continue;
            uint8_t pixels[63*57];assert(native_prompt_raster(&g,pixels,63,57));
            unsigned ink=0;for(unsigned i=0;i<sizeof pixels;i++)ink+=pixels[i]!=0;
            assert(ink>20 && ink<sizeof pixels);
        }
    }
    uint8_t *cache=calloc(1,0x1eb0),*atlas=calloc(128,128),entry[0x60]={0},manager[0x50]={0},worker[0xe8]={0};
    char name[50]="External";assert(cache&&atlas);
    put32(cache+0x28,128);put32(cache+0x2c,128);put32(cache+0x1880,1);putptr(cache+0x18,name);
    putptr(cache+0x1e98,atlas);putptr(cache+0x58,cache+0x50);
    putptr(cache+0x80,entry+0x10);putptr(cache+0x88,entry+0x10);
    putptr(entry+0x10,cache+0x80);putptr(entry+0x18,cache+0x80);
    put32(entry+0x30,0xe040);put16(entry+0x40,20);put16(entry+0x42,25);put16(entry+0x44,40);put16(entry+0x46,50);
    for(unsigned y=10;y<48;y++)for(unsigned x=2;x<38;x++)atlas[(127-25-y)*128+21+x]=255;
    putptr(manager+0x10,cache);putptr(manager+0x28,worker);
    native_prompt_atlas_bind(map,no_update,no_update);
    native_prompts_use(NATIVE_PROMPT_KEYBOARD,false);native_prompt_manager_update(manager);assert(writes);
    uint8_t keyboard[128*128];memcpy(keyboard,atlas,sizeof keyboard);
    writes=0;native_prompt_manager_update(manager);assert(!writes);
    // A connected controller changes a prompt which is already in the cache.
    native_prompts_use(NATIVE_PROMPT_PLAYSTATION,false);native_prompt_manager_update(manager);
    assert(writes && memcmp(keyboard,atlas,sizeof keyboard));
    // Switching while a font job is pending must wait for its completion.
    native_prompts_use(NATIVE_PROMPT_KEYBOARD,false);worker[0xe0]=1;writes=0;
    native_prompt_manager_update(manager);assert(!writes);worker[0xe0]=0;
    native_prompt_manager_update(manager);assert(writes && !memcmp(keyboard,atlas,sizeof keyboard));
    // Unrelated fonts using the same private code point keep their pixels.
    strcpy(name,"StorySymbols");native_prompts_use(NATIVE_PROMPT_SWITCH,false);writes=0;
    native_prompt_manager_update(manager);assert(!writes && !memcmp(keyboard,atlas,sizeof keyboard));
    // All writes stay in the glyph's rectangle, including top-down atlas rows.
    for(unsigned y=0;y<128;y++)for(unsigned x=0;x<128;x++)
        if(x<21 || x>=61 || y<53 || y>=103)assert(!atlas[y*128+x]);
    strcpy(name,"External");pending_entry=entry;
    native_prompt_atlas_bind(map,finish_pending,no_update);
    for(unsigned failure=2;failure<=3;failure++){
        pending_failure=failure;
        putptr(cache+0x58,entry);putptr(entry+8,cache+0x50);
        putptr(cache+0x88,cache+0x80);
        writes=0;native_prompt_cache_update(cache);
        assert(!writes && !memcmp(keyboard,atlas,sizeof keyboard));
    }
    pending_failure=0;putptr(cache+0x58,entry);putptr(entry+8,cache+0x50);
    writes=0;native_prompt_cache_update(cache);
    assert(writes && memcmp(keyboard,atlas,sizeof keyboard));
    free(cache);free(atlas);
    puts("Input bindings, five device families, glyph raster and live atlas switching: PASS");
}
