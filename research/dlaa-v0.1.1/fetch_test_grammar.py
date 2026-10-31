"""Fetch the pinned Khronos grammar into ignored dependencies, without vendoring it."""
import hashlib
import urllib.request
from pathlib import Path

COMMIT = 'fef168c54ed2e37c314eb251806aaf9877f38465'
SHA256 = 'ad92bc42e8ed893cb99d1c2c4a3ff48eaff57211d88706f044f160bbf3bbc363'
URL = f'https://raw.githubusercontent.com/KhronosGroup/SPIRV-Headers/{COMMIT}/include/spirv/unified1/spirv.core.grammar.json'
target = Path(__file__).parent / 'dependencies/spirv.core.grammar.json'
data = target.read_bytes() if target.exists() else urllib.request.urlopen(URL).read()
if hashlib.sha256(data).hexdigest() != SHA256:
    raise ValueError('Khronos grammar checksum mismatch')
target.parent.mkdir(parents=True, exist_ok=True)
target.write_bytes(data)
print('Pinned Khronos grammar verified')
