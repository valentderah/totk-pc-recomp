import os
import ctypes
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = Path(os.environ['TOTK_DLAA_PACKAGE'])

class DlaaMenuTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        harness = ROOT / 'build/dlaa_menu_test.c'
        dll = ROOT / 'build/dlaa_menu_test.dll'
        harness.write_text('''
#include <stdio.h>
#include <string.h>
#include "../native/dlaa_settings.h"
static char value[64]="K"; static int saves;
const char *cfg_value(const char *key){return value;}
void cfg_set(const char *key,const char *v){snprintf(value,sizeof value,"%s",v);}
int cfg_save(void){saves++;return 1;}
__declspec(dllexport) void select_value(const char *v){snprintf(value,sizeof value,"%s",v);}
__declspec(dllexport) int initialize(const char *seed){saves=0;return dlaa_settings_init(seed);}
__declspec(dllexport) unsigned requested(void){return dlaa_settings_requested();}
__declspec(dllexport) void evaluated(int ok){dlaa_settings_evaluated(ok);}
__declspec(dllexport) int save_count(void){return saves;}
__declspec(dllexport) const char *note(void){static char b[256];dlaa_settings_note(b,sizeof b);return b;}
''')
        subprocess.run([str(PACKAGE/'llvm/bin/clang.exe'), '-shared', '-O2',
                        '-I', str(PACKAGE/'runtime'), str(harness), str(ROOT/'native/dlaa_settings.c'),
                        '-o', str(dll)], check=True, capture_output=True)
        cls.dll = ctypes.CDLL(str(dll))
        cls.dll.initialize.argtypes = [ctypes.c_char_p]
        cls.dll.select_value.argtypes = [ctypes.c_char_p]
        cls.dll.note.restype = ctypes.c_char_p

    def setUp(self):
        self.dll.select_value(b'K')
        self.assertEqual(self.dll.initialize(None), 1)

    def note(self):
        return self.dll.note().decode('utf8')

    def test_menu_change_before_sdk_does_not_change_boot_request(self):
        self.dll.select_value(b'L')
        self.assertEqual(self.dll.requested(), 11)
        self.assertIn('Restart', self.note())
        self.assertNotIn('Active', self.note())

    def test_active_only_after_success_and_failure_clears_it(self):
        self.assertIn('Waiting', self.note())
        self.dll.evaluated(1)
        self.assertIn('Active: K', self.note())
        self.dll.evaluated(0)
        self.assertIn('unavailable', self.note())
        self.assertNotIn('Active', self.note())

    def test_change_and_revert_updates_pending_without_reconfiguring(self):
        self.dll.evaluated(1)
        self.dll.select_value(b'L')
        self.assertIn('Active: K', self.note())
        self.assertIn('Restart', self.note())
        self.dll.select_value(b'11')
        self.assertNotIn('Restart', self.note())
        self.assertEqual(self.dll.requested(), 11)

    def test_seed_is_saved_once_and_next_launch_honors_it(self):
        self.assertEqual(self.dll.initialize(b'L'), 1)
        self.assertEqual(self.dll.requested(), 12)
        self.assertEqual(self.dll.save_count(), 1)
        self.assertEqual(self.dll.initialize(None), 1)
        self.assertEqual(self.dll.requested(), 12)
        self.assertEqual(self.dll.save_count(), 0)

    def test_invalid_seed_does_not_modify_saved_choice(self):
        self.assertEqual(self.dll.initialize(b'M'), 0)
        self.assertEqual(self.dll.save_count(), 0)
        self.assertEqual(self.dll.initialize(None), 1)
        self.assertEqual(self.dll.requested(), 11)

    def test_invalid_config_is_rejected(self):
        self.dll.select_value(b'garbage')
        self.assertEqual(self.dll.initialize(None), 0)

class LauncherEnvironmentTests(unittest.TestCase):
    def test_saved_selection_removes_override_instead_of_setting_empty(self):
        shell = Path(os.environ.get('TOTK_DLAA_PWSH','pwsh'))
        script = r'''
$root = $args[0]
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'launch_dlaa_test.ps1'),[ref]$null,[ref]$null)
$loops = $ast.FindAll({param($a) $a -is [System.Management.Automation.Language.ForEachStatementAst]},$true)
$setup = $loops | Where-Object {$_.Condition.Extent.Text -eq '$settings.Keys'} | Select-Object -First 1
$restore = $loops | Where-Object {$_.Condition.Extent.Text -eq '$prior.Keys'} | Select-Object -First 1
$settings = @{TOTK_DLAA_PRESET=$null; TOTK_DLAA_INITIAL_PRESET='K'}
$prior = @{}
[Environment]::SetEnvironmentVariable('TOTK_DLAA_PRESET','old','Process')
Remove-Item Env:TOTK_DLAA_INITIAL_PRESET -ErrorAction SilentlyContinue
& ([scriptblock]::Create($setup.Extent.Text))
if(Test-Path Env:TOTK_DLAA_PRESET){throw 'Saved mode leaves an empty environment override'}
if($env:TOTK_DLAA_INITIAL_PRESET -ne 'K'){throw 'Explicit seed missing'}
& ([scriptblock]::Create($restore.Extent.Text))
if($env:TOTK_DLAA_PRESET -ne 'old'){throw 'Existing override not restored'}
if(Test-Path Env:TOTK_DLAA_INITIAL_PRESET){throw 'Originally absent environment entry was not removed'}
'''
        test_script = ROOT/'build/dlaa_menu_environment_test.ps1'
        test_script.write_text(script)
        result = subprocess.run([str(shell), '-NoProfile', '-File', str(test_script), str(ROOT)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

class NgxMenuFailureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        clang = str(PACKAGE/'llvm/bin/clang.exe')
        fake = ROOT/'build/dlaa_menu_reject_bridge.c'
        bridge = ROOT/'build/dlaa_menu_reject_bridge.dll'
        fake.write_text('__declspec(dllexport) int totk_ngx_record_dlaa(void){return 0;}\n'
                        '__declspec(dllexport) int totk_ngx_configure_replay_preset(unsigned p){return 0;}\n')
        subprocess.run([clang,'-shared',str(fake),'-o',str(bridge)],check=True,capture_output=True)
        harness = ROOT/'build/dlaa_menu_failure_test.c'
        dll = ROOT/'build/dlaa_menu_failure_test.dll'
        harness.write_text(r'''
#include "../native/ngx_probe.c"
#include "../native/dlaa_settings.c"
const char *cfg_value(const char *k){return "K";}
void cfg_set(const char *k,const char *v){}
int cfg_save(void){return 1;}
void rt_log(const char *fmt,...){}
void rt_fatal(const char *fmt,...){abort();}
VkCtx *vk_ctx(void){static VkCtx ctx;return &ctx;}
int vk_projection_probe_sequence_enabled(void){return 0;}
__declspec(dllexport) const char *failure_case(int which,const char *bridge){
    if(bridge_module){FreeLibrary(bridge_module);bridge_module=NULL;}
    const char *keys[]={"TOTK_NGX_EVALUATE","TOTK_DLAA_PREVIEW","TOTK_NGX_SEQUENCE","TOTK_NGX_BRIDGE","TOTK_NGX_RUNTIME_DIR","TOTK_DUMP_DIR"};
    char *prior[6];for(int i=0;i<6;i++){const char *v=getenv(keys[i]);prior[i]=v?_strdup(v):NULL;}
    resident_after_failure=eval_pending=0;
    _putenv_s("TOTK_NGX_EVALUATE","1");_putenv_s("TOTK_DLAA_PREVIEW","1");
    _putenv_s("TOTK_NGX_SEQUENCE","");
    _putenv_s("TOTK_NGX_BRIDGE",which==0?"":which==1?"Z:/missing-bridge.dll":which==2?"kernel32.dll":bridge);
    _putenv_s("TOTK_NGX_RUNTIME_DIR","unused");_putenv_s("TOTK_DUMP_DIR","unused");
    dlaa_settings_init(NULL);
    if(ngx_probe_record_eval(NULL)>=0)abort();
    static char note[256];dlaa_settings_note(note,sizeof note);
    for(int i=0;i<6;i++){_putenv_s(keys[i],prior[i]?prior[i]:"");free(prior[i]);}
    return note;
}
''')
        result = subprocess.run([clang,'-shared','-O2','-I',str(PACKAGE/'runtime'),
                                 '-I',str(PACKAGE/'runtime/deps/include'),'-I',str(PACKAGE/'runtime/deps/include/vma'),str(harness),
                                 '-L',str(PACKAGE/'runtime/deps/lib'),'-lvulkan-1','-o',str(dll)],capture_output=True,text=True)
        if result.returncode:
            raise AssertionError(result.stderr)
        cls.dll = ctypes.CDLL(str(dll))
        cls.dll.failure_case.argtypes = [ctypes.c_int,ctypes.c_char_p]
        cls.dll.failure_case.restype = ctypes.c_char_p
        cls.bridge = str(bridge).encode()

    def test_every_pre_record_failure_marks_dlaa_unavailable(self):
        for case in range(4):
            with self.subTest(case=case):
                note = self.dll.failure_case(case,self.bridge).decode()
                self.assertIn('unavailable',note)
                self.assertNotIn('Waiting',note)
