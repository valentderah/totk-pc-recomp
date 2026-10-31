"""Execute the actual local game's adaptive and emergency size producers."""
import ctypes
import ast
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X19, UC_ARM64_REG_W0, UC_ARM64_REG_W8

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / 'generated/macos-arm64/v121/main.patched.bin'
NATIVE_TREE = ROOT / 'build/macos-arm64/source/games/totk'


@unittest.skipUnless(NATIVE_TREE.is_dir(), 'prepared native sources required')
class ResolutionHeapTests(unittest.TestCase):
    def test_runtime_allocation_matches_romfs_heap_in_both_modes(self):
        # Execute the actual overlay generator's calculation without its CLI.
        module = ast.parse((NATIVE_TREE / 'scripts/make_output_res_overlay.py').read_text())
        names = {'BASE_W', 'BASE_H', 'DTA_KIB', 'GRAPHICS_KIB'}
        nodes = []
        for node in module.body:
            if isinstance(node, ast.FunctionDef) and node.name == 'extra_kib':
                nodes.append(node)
            elif isinstance(node, ast.Assign) and any(
                    isinstance(item, ast.Name) and item.id in names for item in ast.walk(node)):
                nodes.append(node)
        import math
        overlay = {'math': math}
        exec(compile(ast.Module(nodes, type_ignores=[]), '<overlay heap calculation>', 'exec'), overlay)
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as directory:
            directory = Path(directory)
            code = ['#include <stdint.h>\n#include <math.h>\n'
                    'static unsigned width,height; static int strict;\n'
                    'int runtime_output_res(unsigned *w,unsigned *h){*w=width;*h=height;return 1;}\n'
                    'int native_policy_strict(void){return strict;}\n'
                    'void size_mode(unsigned w,unsigned h,int s){width=w;height=h;strict=s;}\n']
            for version, name in [('v121', 'game_patches.c'), ('v143', 'game_patches_v143.c')]:
                source = (NATIVE_TREE / 'runtime' / name).read_text()
                start = source.index('uint64_t runtime_output_res_extra_bytes(')
                end = source.index('\n}', start) + 2
                code.append(source[start:end].replace('runtime_output_res_extra_bytes', f'extra_{version}'))
            (directory / 'heap.c').write_text('\n'.join(code))
            library = directory / 'heap.dylib'
            subprocess.run(['xcrun', 'clang', '-dynamiclib', directory / 'heap.c', '-o', library], check=True)
            native = ctypes.CDLL(str(library))
            for version in ('v121', 'v143'):
                function = getattr(native, f'extra_{version}')
                function.restype = ctypes.c_uint64
                for w, h in ((1280,720), (1920,1080), (2560,1440), (3840,2160), (3840,2159), (7680,4320)):
                    for strict in (0,1):
                        with self.subTest(version=version, size=(w,h), strict=strict):
                            native.size_mode(w,h,strict)
                            self.assertEqual(function(), overlay['extra_kib'](w,h)*1024)


@unittest.skipUnless(IMAGE.is_file(), 'verified local 1.2.1 modules required')
class FixedResolutionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(dir=ROOT / 'build')
        directory = Path(cls.temporary.name)
        shim = directory / 'shim.c'
        shim.write_text('#include <stdlib.h>\nint native_policy_strict(void){return 1;}\n'
                        '_Noreturn void runtime_fail(const char *message){(void)message;abort();}\n')
        library = directory / 'resolution.dylib'
        subprocess.run(['xcrun', 'clang', '-dynamiclib', '-DTOTK_GAME_V121=1',
                        ROOT / 'runtime/macos/resolution.c', shim, '-o', library], check=True)
        cls.library = ctypes.CDLL(str(library))
        cls.library.native_patch_main.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        cls.original = IMAGE.read_bytes()
        image = ctypes.create_string_buffer(cls.original, len(cls.original))
        cls.library.native_patch_main(image, len(image))
        cls.patched = image.raw

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def scale(self, image, start, end, quality=1.0, flags=0):
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        cpu.mem_map(0xcc0000, 0x10000)
        cpu.mem_write(start, image[start:end])
        renderer = 0x10000000
        cpu.mem_map(renderer, 0x2000)
        cpu.mem_write(renderer + 0xcc0, struct.pack('<4f', 3840, 3840, 2160, -1))
        cpu.reg_write(UC_ARM64_REG_X19, renderer)
        cpu.reg_write(UC_ARM64_REG_W0, struct.unpack('<I', struct.pack('<f', quality))[0])
        cpu.reg_write(UC_ARM64_REG_W8, flags)
        cpu.emu_start(start, end, count=16)
        return struct.unpack('<f', cpu.mem_read(renderer + 0xccc, 4))[0]

    def test_adaptive_quality_cannot_reduce_the_scene(self):
        for quality in (0.25, 0.5, 0.89765, 1.0):
            with self.subTest(quality=quality):
                self.assertAlmostEqual(self.scale(self.original, 0xcc1138, 0xcc1154, quality), quality)
                self.assertEqual(self.scale(self.patched, 0xcc1138, 0xcc1154, quality), 1.0)

    def test_emergency_quality_cannot_reduce_the_scene(self):
        for flags, original in ((0, 0.75), (0x20, 0.5)):
            with self.subTest(flags=flags):
                self.assertEqual(self.scale(self.original, 0xcc17cc, 0xcc17f0, flags=flags), original)
                self.assertEqual(self.scale(self.patched, 0xcc17cc, 0xcc17f0, flags=flags), 1.0)

    def test_only_the_two_scale_producers_change(self):
        changed = {i for i, (a, b) in enumerate(zip(self.original, self.patched)) if a != b}
        allowed = {p + i for p in (0xcc114c, 0xcc17e8) for i in range(4)}
        self.assertTrue(changed)
        self.assertLessEqual(changed, allowed)


if __name__ == '__main__':
    unittest.main()
