"""The independent-controls text uses complete menu catalogs and follows live language changes."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Catalog(unittest.TestCase):
    def test_control_strings_in_every_menu_language(self):
        source = r'''
#include "ui_text.h"
#include <assert.h>
#include <string.h>
static const char *language="en";
static unsigned version;
const char *ov_language(void){return "en";}
const char *plat_ui_language(void){return "en";}
const char *cfg_value(const char *key){(void)key;return language;}
unsigned cfg_version(void){return version;}
int main(void){
    const char *langs[]={"en","ru","pt-BR","zh-Hans"};
    for(unsigned l=0;l<4;l++) {
        for(int id=UI_ACTION_LABEL_JUMP;id<=UI_ACTION_COMMON_HINT;id++) {
            assert(ui_text_defined(langs[l],id));
            const char *text=ui_text_in(langs[l],id);
            assert(*text && strlen(text)<UI_DESC_BYTES-128);
        }
        language=langs[l];version++;
        assert(!strcmp(menu_text(UI_ACTION_LABEL_SCOPE),ui_text_in(langs[l],UI_ACTION_LABEL_SCOPE)));
    }
    assert(!strcmp(ui_text_in("en",UI_ACTION_LABEL_SCOPE),"Scope (open / close)"));
    assert(strcmp(ui_text_in("en",UI_ACTION_ENABLE),ui_text_in("ru",UI_ACTION_ENABLE)));
    return 0;
}
'''
        workroot = ROOT / 'build/tests/independent-controls'
        workroot.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=workroot) as td:
            p = Path(td)
            (p / 'catalog.c').write_text(source)
            subprocess.run(['clang', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'runtime'),
                            str(p / 'catalog.c'), str(ROOT / 'runtime/ui_text.c'), '-o', str(p / 'catalog')], check=True)
            subprocess.run([str(p / 'catalog')], check=True)


if __name__ == '__main__':
    unittest.main()
