"""Keep fixed development cache overrides isolated across shader generations."""
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from macos.build import BUILD
from macos.shaders import metal_compiler_identity, metal_version


def check_compiler_identity():
    version = 'Apple metal version 32023.921 (metalfe-32023.921.6)\nTarget: air64-apple-darwin27.2.0\nThread model: posix\n'
    def identity(text):
        with patch('macos.shaders.subprocess.check_output', return_value=text):
            return metal_compiler_identity()
    a = version + 'InstalledDir: /private/var/run/cryptex/session-a/Metal.xctoolchain/bin\n'
    b = version + 'InstalledDir: /private/var/run/cryptex/session-b/Metal.xctoolchain/bin\n'
    assert identity(a) == identity(b), 'reboot invalidated identical compiler output'
    assert identity(a) != identity(a.replace('32023.921.6', '32023.921.7')), 'compiler update reused old libraries'
    assert identity(a) != identity(a.replace('darwin27.2.0', 'darwin27.3.0')), 'target update reused old libraries'
    original = identity(a)
    with patch('macos.shaders.METAL_FLAGS', ('-std=metal3.2', '-fmetal-math-mode=fast')):
        assert identity(a) != original, 'arithmetic options reused old libraries'
    with patch('macos.shaders.subprocess.check_output', return_value='InstalledDir: /temporary\n'):
        try:
            metal_version()
        except RuntimeError:
            pass
        else:
            raise AssertionError('empty compiler identity was accepted')
    print('{"probe":"metal-compiler-identity","status":"pass","checks":5}')


def main():
    check_compiler_identity()
    with tempfile.TemporaryDirectory(dir=BUILD, prefix='shader-cache-test-') as tmp:
        work = Path(tmp)
        source = work / 'cache.m'
        source.write_text('''#include "native_shader_cache.h"
#include <assert.h>
int main(void) { @autoreleasepool {
    NSString *a=[@"a" stringByPaddingToLength:64 withString:@"a" startingAtIndex:0];
    NSString *b=[@"b" stringByPaddingToLength:64 withString:@"b" startingAtIndex:0];
    NSString *base=@"/fixed/shaders", *first=native_shader_cache_resolve(base,a,@"26A1",42);
    assert(([first isEqual:[base stringByAppendingFormat:@"/%@-26A1-2a",a]]));
    assert([native_shader_cache_resolve(first,a,@"26A1",42) isEqual:first]);
    assert(![native_shader_cache_resolve(base,b,@"26A1",42) isEqual:first]);
    assert(![native_shader_cache_resolve(base,a,@"26A2",42) isEqual:first]);
    assert(![native_shader_cache_resolve(base,a,@"26A1",43) isEqual:first]);
    assert(!native_shader_cache_resolve(base,@"invalid",@"26A1",42));
    assert(!native_shader_cache_resolve(base,[a stringByReplacingCharactersInRange:NSMakeRange(0,1) withString:@"/"],@"26A1",42));
    assert(!native_shader_cache_resolve(base,a,@"",42));
    puts("{\\"probe\\":\\"shader-cache-generation\\",\\"status\\":\\"pass\\",\\"checks\\":8}");
    return 0;
}}
''')
        executable = work / 'cache'
        subprocess.run(['xcrun', 'clang', '-O2', '-fobjc-arc', '-mmacosx-version-min=26.0',
                        '-I', ROOT / 'runtime/macos', '-framework', 'Foundation', '-framework', 'Metal',
                        source, '-o', executable], check=True)
        subprocess.run([executable], check=True)


if __name__ == '__main__':
    main()
