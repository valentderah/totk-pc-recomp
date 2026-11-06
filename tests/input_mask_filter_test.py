"""Check opt-in publisher filtering with synthetic masks and no game data."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class Filter(unittest.TestCase):
    def test_explicit_site_and_invalid_configuration(self):
        with tempfile.TemporaryDirectory() as td:
            folder = Path(td)
            source = folder / 'filter.c'
            source.write_text('''#include <stdint.h>
#include <stdio.h>
int64_t frame_step_world_us(void) { return -1; }
void __real_input_poll(uint64_t *b, int32_t *x, int32_t *y, int32_t *z, int32_t *w) {}
uint64_t input_output_filter(uint64_t, uint64_t, uint64_t);
int main(void) {
    printf("%llu %llu\\n", (unsigned long long)input_output_filter(4096, 40, 0x100000003ull),
           (unsigned long long)input_output_filter(8192, 40, 0x100000003ull));
}
''')
            executable = folder / 'filter'
            subprocess.run(['clang', str(source), str(ROOT / 'tools/input_query_logger.c'), '-lpthread',
                            '-o', str(executable)], check=True)
            env = dict(os.environ)
            env.pop('TOTK_QUERY_LOG', None)
            for config, value in [(None, 0x100000003), ('0x1000:0x100000002', 1),
                                  ('0x2000:2', 0x100000003), ('4096:2garbage', 0x100000003),
                                  ('4096:-1', 0x100000003), ('-1:2', 0x100000003),
                                  ('4096:0x10000000000000000', 0x100000003)]:
                with self.subTest(config=config):
                    env.pop('TOTK_MASK_CLEAR', None)
                    if config is not None:
                        env['TOTK_MASK_CLEAR'] = config
                    result = subprocess.run([str(executable)], env=env, check=True, capture_output=True, text=True)
                    first, other = map(int, result.stdout.split())
                    self.assertEqual(first, value)
                    self.assertEqual(other, 0x100000001 if config == '0x2000:2' else 0x100000003)


if __name__ == '__main__':
    unittest.main()
