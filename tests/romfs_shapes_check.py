"""Exercise runtime/romfs.c shape lookup and generator commands without game data.

Usage: python tests/romfs_shapes_check.py [--cc C:/msys64/ucrt64/bin/clang.exe]
The C harness compiles function bodies read from the production implementation;
only configuration, filesystem and process boundaries are supplied here. The spawn boundary records arguments.
"""
import argparse
import os
import re
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <assert.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "shape_functions.h"
const int rt_game_version = 143;
static char index_test[2400];
static const char *hud_test="16:9", *override_test;
static int spawns;
static char args_test[24][2400];
const char *cfg_get(const char *key) {
    if (!strcmp(key,"TOTK_ROMFS_INDEX")) return index_test;
    if (!strcmp(key,"TOTK_ROMFS_PATCHES")) return override_test;
    return NULL;
}
const char *cfg_value(const char *key) { return !strcmp(key,"TOTK_HUD_WIDTH") ? hud_test : ""; }
// Fixed aliases; cfg_probe independently checks the real canonicalization algorithm.
int cfg_shape_size(unsigned w,unsigned h,unsigned out[2]) {
    if (w*9ull==h*16ull) return 0;
    if (w==5120 && h==2160) out[0]=2560,out[1]=1080;
    else if (w==5120 && h==1440) out[0]=3840,out[1]=1080;
    else out[0]=w,out[1]=h;
    return 1;
}
int res_wants_wide_overlay(const unsigned out[2]) { return out[0]*9ull!=out[1]*16ull; }
int plat_file_exists(const char *p) { struct stat s; return !stat(p,&s) && S_ISREG(s.st_mode); }
uint8_t *plat_file_read(const char *p,uint64_t *size) {
    FILE *f=fopen(p,"rb"); if (!f) return NULL;
    fseek(f,0,SEEK_END); *size=ftell(f); rewind(f);
    uint8_t *b=calloc(1,*size+1); assert(b); assert(fread(b,1,*size,f)==*size); fclose(f); return b;
}
int plat_exe_path(char *out,size_t cap) { (void)out;(void)cap;abort(); }
void rt_fatal(const char *fmt,...) { (void)fmt;abort(); }
void rt_log(const char *fmt,...) { (void)fmt; }
int plat_spawn(const char *const *args,const char *cwd) {
    assert(cwd && *cwd); spawns++;
    int i; for (i=0;args[i];i++) { assert(i<23); snprintf(args_test[i],sizeof args_test[i],"%s",args[i]); }
    args_test[i][0]=0; return 1;
}
static const char *arg(const char *key) {
    for (int i=0;args_test[i][0];i++) if (!strcmp(args_test[i],key)) return args_test[i+1];
    return NULL;
}
static void output_ends(const char *suffix) {
    const char *p=arg("--out"); assert(p && strlen(p)>=strlen(suffix));
    assert(!strcmp(p+strlen(p)-strlen(suffix),suffix));
}
int main(int argc,char **argv) {
    assert(argc==2); char base[2400],name[128];
    snprintf(base,sizeof base,"%s/selection",argv[1]);
    snprintf(index_test,sizeof index_test,"%s/romfs-index.bin",base);
    assert(selected_shape(base,"romfs-patches",5120,2160,name,sizeof name,1));
    assert(!strcmp(name,"romfs-patches-2560x1080-hud16x9")); // selected canonical beats exact full
    assert(selected_shape(base,"romfs-patches-xbox",5120,2160,name,sizeof name,0));
    assert(!strcmp(name,"romfs-patches-xbox-5120x2160-hud16x9")); // exact cap beats canonical cap
    hud_test="21:9";
    assert(selected_shape(base,"romfs-patches",5120,1440,name,sizeof name,1));
    assert(!strcmp(name,"romfs-patches-3840x1080"));
    assert(!selected_shape(base,"romfs-patches",5120,1440,name,sizeof name,0));
    assert(romfs_shape_missing(5120,1440,NULL));
    override_test="0"; assert(!romfs_shape_missing(5120,1440,NULL)); override_test=NULL;
    assert(!romfs_shape_missing(1920,1080,NULL));
    snprintf(index_test,sizeof index_test,"%s/generation/romfs-index.bin",argv[1]);
    hud_test="16:9";
    assert(romfs_shape_prepare(5120,2160,"romfs-patches-pc")); assert(spawns==2);
    assert(!strcmp(arg("--aspect"),"2560x1080")); assert(!strcmp(arg("--hud"),"16:9"));
    assert(!strcmp(arg("--version"),"v143")); assert(!strcmp(arg("--prompts"),"pc"));
    assert(arg("--reuse") && strstr(arg("--reuse"),"/romfs-patches-pc"));
    assert(arg("--binds") && strstr(arg("--binds"),"/totk.ini")); assert(!args_test[18][0]); output_ends("romfs-patches-pc-2560x1080-hud16x9");
    assert(romfs_shape_prepare(5120,2160,"romfs-patches-pc")); assert(spawns==2);
    hud_test="full";
    assert(romfs_shape_prepare(5120,2160,"romfs-patches-xbox")); assert(spawns==4);
    assert(!strcmp(arg("--hud"),"full")); assert(!strcmp(arg("--prompts"),"xbox"));
    assert(arg("--reuse") && !arg("--binds")); output_ends("romfs-patches-xbox-2560x1080");
    hud_test="21:9";
    assert(romfs_shape_prepare(5120,1440,"romfs-patches-xbox-label")); assert(spawns==6);
    assert(!strcmp(arg("--hud"),"21:9")); assert(!strcmp(arg("--prompts"),"xbox-label"));
    assert(!arg("--reuse") && !arg("--binds")); output_ends("romfs-patches-xbox-label-3840x1080-hud21x9");
    // A cap wider than a tall canvas becomes full, never an invalid generator request.
    assert(romfs_shape_prepare(1280,800,NULL)); assert(spawns==7);
    assert(!strcmp(arg("--hud"),"full")); output_ends("romfs-patches-1280x800");
    // Beyond the old eight-folder limit every new folder must still be remembered.
    const char *dummy[]={"python","script",NULL};
    for (int i=0;i<12;i++) {
        char folder[80]; snprintf(folder,sizeof folder,"additional-request-%d",i);
        int before=spawns; assert(shape_spawn(dummy,argv[1],folder)); assert(spawns==before+1);
        assert(shape_spawn(dummy,argv[1],folder)); assert(spawns==before+1);
    }
    puts("PASS production shape selection, command arguments and generation deduplication");
}
'''


def production_functions():
    """Compile the current function bodies, without duplicating their implementation.

    MinGW resolves unrelated public RomFS entry points before section GC, so including
    the complete unit would require fake audio, mod, decompression and filesystem APIs.
    """
    source = (ROOT / 'runtime/romfs.c').read_text()
    names = ('need', 'hud_suffix', 'shaped_name', 'selected_shape', 'index_path', 'index_folder',
             'shaped_folder_ready', 'romfs_shaped_ready', 'romfs_shaped_prompts_ready', 'prepare_tools',
             'romfs_shape_missing', 'shape_was_started', 'shape_spawn', 'romfs_shape_prepare')
    state = re.findall(r'(?m)^static [^;\n]*\bshape_started[^;\n]*;', source)
    assert len(state) == 2, 'update harness extraction for changed generation cache declarations'
    functions = []
    for name in names:
        match = re.search(r'(?m)^[^\n;{}]*\b' + name + r'\([^;{}]*?\)\s*\{', source)
        assert match, f'production function not found: {name}'
        end = match.end()
        depth = 1
        while depth:
            assert end < len(source), f'unclosed production function: {name}'
            if source[end] == '{': depth += 1
            elif source[end] == '}': depth -= 1
            end += 1
        functions.append(source[match.start():end])
    return '\n'.join(state + functions)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--cc', default='C:/msys64/ucrt64/bin/clang.exe' if os.name == 'nt' else 'clang')
    args = ap.parse_args()
    work = ROOT / 'build' / 'romfs-shapes-check'
    work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=work) as temporary:
        folder = Path(temporary)
        for name in ('romfs-patches-5120x2160', 'romfs-patches-2560x1080-hud16x9',
                     'romfs-patches-xbox-5120x2160-hud16x9', 'romfs-patches-xbox-2560x1080-hud16x9',
                     'romfs-patches-3840x1080'):
            manifest = folder / 'selection' / name / 'manifest.tsv'
            manifest.parent.mkdir(parents=True, exist_ok=True)
            manifest.write_text('# synthetic\n')
        generation = folder / 'generation'
        for name in ('build-cache', 'romfs', 'tools', 'romfs-patches-pc', 'romfs-patches-xbox'):
            (generation / name).mkdir(parents=True, exist_ok=True)
        for name in ('romfs/romfs.bin', 'tools/make_romfs_patches.py', 'fake-python', 'totk.ini',
                     'romfs-patches-pc/manifest.tsv', 'romfs-patches-xbox/manifest.tsv'):
            (generation / name).write_text('# synthetic\n')
        (generation / 'build-cache/rebuild.txt').write_text(
            f'{(generation / "fake-python").as_posix()}\n-c\nunused\n{generation.as_posix()}\n')
        (folder / 'shape_functions.h').write_text(production_functions())
        source = folder / 'harness.c'
        source.write_text(HARNESS)
        exe = folder / ('harness.exe' if os.name == 'nt' else 'harness')
        command = [args.cc, '-O2', '-flto', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                   '-I', str(folder), '-I', str(ROOT / 'runtime'), str(source), '-o', str(exe)]
        subprocess.run(command, check=True, cwd=ROOT)
        env = os.environ.copy()
        if os.name == 'nt': env['PATH'] = str(Path(args.cc).resolve().parent) + os.pathsep + env.get('PATH', '')
        subprocess.run([str(exe), folder.as_posix()], check=True, cwd=ROOT, env=env)


if __name__ == '__main__': main()
