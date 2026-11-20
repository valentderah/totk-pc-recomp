// Policy and owner-wiring regression; portable C, no game, GPU or window required.
// From the repository root, create build/test (or choose another output directory), then:
// clang -O2 -Wall -Wextra -Werror -I runtime tests/perf_mode_probe.c -o build/test/perf_mode_probe
// Run: build/test/perf_mode_probe [runtime/gfx_present.c runtime/ui/menu.cpp]
// The source checks verify the real owner call precedes skipped-frame exits; they do not execute Vulkan/ImGui.
#include <stdio.h>
#include <stdlib.h>
#include "ui/perf_mode.h"

static char *source(const char *path) {
    FILE *file=fopen(path,"rb");
    if(!file) { printf("FAIL cannot read %s\n",path); return NULL; }
    if(fseek(file,0,SEEK_END)) { fclose(file); return NULL; }
    long size=ftell(file);
    if(size<0 || fseek(file,0,SEEK_SET)) { fclose(file); return NULL; }
    char *text=malloc((size_t)size+1);
    if(!text) { fclose(file); return NULL; }
    size_t read=fread(text,1,(size_t)size,file); fclose(file);
    if(read!=(size_t)size) { free(text); return NULL; }
    text[size]=0; return text;
}
static int owner_wiring(const char *gfx_path, const char *menu_path) {
    char *gfx=source(gfx_path), *menu=source(menu_path);
    int failed=0;
    if(!gfx || !menu) { free(gfx); free(menu); return 1; }
    char *present=strstr(gfx,"static void present_image(");
    char *lock=present?strstr(present,"plat_rmutex_lock(&g_pl);"):NULL;
    char *update=lock?strstr(lock,"menu_perf_update(c->phys);"):NULL;
    char *skip=present?strstr(present,"goto out;"):NULL;
    char *unlock=lock?strstr(lock,"plat_rmutex_unlock(&g_pl);"):NULL;
    if(!lock || !update || !skip || !unlock || update>skip || update>unlock) {
        puts("FAIL skipped presents bypass the serialized sampling owner update"); failed++;
    }
    char *control=strstr(menu,"extern \"C\" void menu_perf_update(VkPhysicalDevice phys)");
    char *fps=control?strstr(control,"static void perf_tick("):NULL;
    char *tick=control?strstr(control,"perf_panel_tick();"):NULL;
    char *mode=control?strstr(control,"perfmon_mode(sampling);"):NULL;
    char *gpu=control?strstr(control,"perf_gpu_once(phys);"):NULL;
    if(!control || !fps || !tick || !mode || !gpu || tick>fps || mode>fps || gpu>fps) {
        puts("FAIL sampling control still depends on a drawable overlay target"); failed++;
    }
    if(!failed) puts("PASS source wiring: physical-device sampling owner precedes every present skip under the present lock");
    free(gfx); free(menu); return failed;
}
int main(int argc, char **argv) {
    const char *values[] = {"off", "fps", "full"};
    int failures = 0;
    for (int display=0; display<3; display++) {
        int mode=perf_display_mode(values[display]);
        for (int window=0; window<2; window++) {
            int sample=perf_sampling_mode(mode,window);
            if (mode!=display || sample!=(window?2:display)) failures++;
            printf("display=%s window=%d -> in_game=%d sampling=%d\n",values[display],window,mode,sample);
        }
        if(perf_sampling_mode(mode,0)!=display) failures++;
        if((mode+1)%3 != perf_display_mode(values[(display+1)%3])) failures++;
    }
    if(perf_display_mode(NULL)!=0 || perf_display_mode("invalid")!=0) failures++;
    failures+=owner_wiring(argc>1?argv[1]:"runtime/gfx_present.c",argc>2?argv[2]:"runtime/ui/menu.cpp");
    printf("perf mode probe: %d failures\n",failures); return failures?1:0;
}
