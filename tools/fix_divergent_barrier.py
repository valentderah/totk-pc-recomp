#!/usr/bin/env python3
"""Move a divergent workgroup barrier in a cached compute shader before its early return.

The translator can place OpControlBarrier in the block after `if (local_id != 0) return;`.
Only invocation 0 then reaches the barrier, which deadlocks RCS0 on Intel ANV
(ecode 12:1:8ed9eff2). This writes <cache>/override/<key>.spv, which the runtime
loads before the pack.

usage: fix_divergent_barrier.py <shader-cache.pack> <64-hex-key>
"""
import struct, subprocess, sys, tempfile
from pathlib import Path

pack, key_hex = Path(sys.argv[1]), sys.argv[2]
key = bytes.fromhex(key_hex)
cache = pack.parent
out = cache / "override"
out.mkdir(exist_ok=True)

with pack.open("rb") as f:
    h = f.read(16)
    n = struct.unpack_from("<I", h, 8)[0]
    ents = []
    for _ in range(n):
        b = f.read(48)
        ents.append((b[:32],) + struct.unpack_from("<IIQ", b, 32))
    hit = next((e for e in ents if e[0] == key and e[1] == 1), None)
    if not hit:
        sys.exit("shader key not found in pack")
    f.seek(hit[3])
    spv = f.read(hit[2])

with tempfile.TemporaryDirectory() as td:
    td = Path(td)
    (td / "in.spv").write_bytes(spv)
    asm = subprocess.run(["spirv-dis", str(td / "in.spv"), "-o", str(td / "in.spvasm")], check=True)
    lines = (td / "in.spvasm").read_text().splitlines()
    bar = next((i for i, l in enumerate(lines) if "OpControlBarrier" in l), None)
    if bar is None:
        sys.exit("no OpControlBarrier in shader")
    label = next(l.split()[0] for l in reversed(lines[:bar]) if "= OpLabel" in l)
    b = lines.pop(bar)
    branch = next((i for i in range(len(lines) - 1, -1, -1)
                   if "OpBranchConditional" in lines[i] and label in lines[i]), None)
    if branch is None:
        sys.exit("barrier block has no conditional predecessor")
    at = branch - 1 if branch and "OpSelectionMerge" in lines[branch - 1] else branch
    lines.insert(at, b)
    (td / "out.spvasm").write_text("\n".join(lines) + "\n")
    subprocess.run(["spirv-as", "--target-env", "vulkan1.1", str(td / "out.spvasm"), "-o", str(td / "out.spv")], check=True)
    subprocess.run(["spirv-val", "--target-env", "vulkan1.1", str(td / "out.spv")], check=True)
    data = (td / "out.spv").read_bytes()
    if len(data) != len(spv):
        sys.exit(f"patched module changed size ({len(spv)} -> {len(data)}); not writing")
    (out / f"{key_hex}.spv").write_bytes(data)
print(out / f"{key_hex}.spv")
