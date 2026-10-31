"""Exercise native Maxwell -> SPIR-V -> MSL -> metallib with a synthetic EXIT kernel."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from macos.build import BUILD
from macos.shaders import compile_libraries


def main():
    service = BUILD / 'shader-toolchain/service/totk_xlate'
    cross = BUILD / 'shader-toolchain/spirv-cross-build/spirv-cross'
    with tempfile.TemporaryDirectory(dir=BUILD, prefix='shader-test-') as tmp:
        out = Path(tmp)
        code = out / 'exit.bin'
        # Maxwell scheduling word, then EXIT with predicate PT and condition T.
        # Encoding is described by Ryujinx InstTable/InstExit; no game bytes.
        code.write_bytes(struct.pack('<QQQQ', 0, 0xE30000000007000F, 0xE30000000007000F, 0xE30000000007000F))
        variant = hashlib.sha256(code.read_bytes()).hexdigest() + '-test'
        bad = dict(variant='invalid', stage='unknown')
        good = dict(variant=variant, stage='cs', code=str(code), local=[1, 1, 1])
        r = subprocess.run([service, '--service', '--spirv-cross', cross, '--out', out, '--chain', 'test'],
                           input=json.dumps(bad)+'\n'+json.dumps(good)+'\n', text=True,
                           capture_output=True, timeout=60, check=True)
        replies = [json.loads(line) for line in r.stdout.splitlines()]
        assert len(replies) == 2 and replies[0]['ok'] is False and replies[1]['ok'] is True, replies
        reflection = json.loads((out / (variant + '_b.json')).read_text())
        assert reflection, 'missing reflection'
        source = out / (variant + '_b.metal')
        assert source.is_file()
        duplicate = out / 'duplicate.metal'
        duplicate.write_bytes(source.read_bytes())
        libraries = out / 'libraries'
        assert compile_libraries([source, duplicate], libraries, 2) == 1
        compiled = list(libraries.glob('*.metallib'))
        assert len(compiled) == 1 and compiled[0].stat().st_size > 0
        modified = compiled[0].stat().st_mtime_ns
        assert compile_libraries([source, duplicate], libraries, 2) == 0
        assert compiled[0].stat().st_mtime_ns == modified
        print(json.dumps(dict(status='pass', stages=['native translator', 'SPIRV-Cross', 'Metal compiler'],
                              scope='synthetic EXIT compute shader; no game or GPU execution')))


if __name__ == '__main__': main()
