"""Build a process-local explicit Vulkan failure-injection layer; no game inputs required."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--cc', type=Path, required=True, help='clang or gcc executable')
ap.add_argument('--include', type=Path, required=True, help='directory containing vulkan/vk_layer.h')
ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'swapchain-fail-layer')
a = ap.parse_args()
a.out.mkdir(parents=True, exist_ok=True)
library = a.out.resolve() / ('totk_swapchain_fail.dll' if os.name == 'nt' else 'libtotk_swapchain_fail.so')
subprocess.run([str(a.cc.resolve()), '-shared', '-O2', '-Wall', '-Wextra',
                *([] if os.name == 'nt' else ['-fPIC']), '-I', str(a.include.resolve()),
                str(ROOT / 'tests' / 'vk_swapchain_fail_layer.c'), '-o', str(library)], check=True)
manifest = {'file_format_version': '1.2.0', 'layer': {
    'name': 'VK_LAYER_TOTK_test_swapchain_fail', 'type': 'GLOBAL', 'library_path': str(library),
    'api_version': '1.3.0', 'implementation_version': '1',
    'description': 'Test only: fail a replacement after retiring its old swapchain',
}}
path = a.out / 'VkLayer_totk_test_swapchain_fail.json'
path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
print(path.resolve())
