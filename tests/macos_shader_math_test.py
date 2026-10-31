"""Compare generated precise arithmetic helpers on the native GPU; no game data."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from macos.build import BUILD


def main():
    cross = BUILD / 'shader-toolchain/spirv-cross-build/spirv-cross'
    with tempfile.TemporaryDirectory(dir=BUILD, prefix='shader-math-') as temporary:
        work = Path(temporary)
        source = work / 'math.comp'
        source.write_text('''#version 450
layout(local_size_x=1) in;
layout(std430,binding=0) buffer Data { vec4 x[]; } data;
void main() {
    precise vec4 product = data.x[0] * data.x[1];
    precise vec4 sum = product + data.x[2];
    data.x[3] = sum;
}
''')
        spv = work / 'math.spv'
        subprocess.run(['glslangValidator', '-V', source, '-o', spv], check=True)
        variants = []
        for version in ('20300', '30200'):
            output = work / f'math-{version}.metal'
            subprocess.run([cross, spv, '--msl', '--msl-version', version, '--output', output], check=True)
            variants.append(output)
        assert '[[clang::optnone]] T spvFMul' in variants[0].read_text()
        candidate = variants[1].read_text()
        assert '#pragma metal fp math_mode(safe)' in candidate and '#pragma metal fp contract(off)' in candidate
        assert '[[clang::optnone]] T spvFMul' not in candidate
        executable = work / 'shader-math'
        subprocess.run(['xcrun', 'clang', '-O2', '-fobjc-arc', '-mmacosx-version-min=26.0',
                        '-framework', 'Foundation', '-framework', 'Metal',
                        ROOT / 'tests/macos_shader_math.m', '-o', executable], check=True)
        subprocess.run([executable, *variants], check=True,
                       env=dict(os.environ, MTL_DEBUG_LAYER='1', MTL_DEBUG_LAYER_ERROR_MODE='assert'))


if __name__ == '__main__':
    main()
