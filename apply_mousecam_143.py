#!/usr/bin/env python3
"""
Applies the 1.4.3 mouselook camera patch to the totk-pc-recomp checkout.

  cd C:\\Users\\Chandler\\Documents\\aaa\\totk-pc-recomp
  python apply_mousecam_143.py            (or: python apply_mousecam_143.py path\\to\\repo)

Does two things, each only if it has not been done yet (safe to run twice):
  1. runtime/mousecam.c:      CAM_OFF_V143 0ull -> 0x0071986cull
  2. recomp/patches-v143.txt: adds the "mousecam" patch set (2 call sites) before the spin-wait block
Keeps a .bak copy of each file it changes. Preserves CRLF/LF line endings.
"""
import os
import re
import shutil
import sys

root = sys.argv[1] if len(sys.argv) > 1 else "."
mc = os.path.join(root, "runtime", "mousecam.c")
pt = os.path.join(root, "recomp", "patches-v143.txt")

PATCH_LINES = [
    "# Mouselook camera (runtime/mousecam.c host_camera at page+0xfd0): both bl callers of the 1.4.3 camera update function",
    "# (main+0x71986c; the mod's 1.2.1 entry was 0xa70604) go to the host function, which calls it and rewrites the nine floats.",
    "set mousecam TOTK_MOUSECAM 1",
    "site mousecam main 718b80 9400033b 94cf1d14   # bl camera -> host",
    "site mousecam main 1b87468 97ae4901 947d62da   # bl camera -> host",
    "",
]


def read(p):
    with open(p, "rb") as f:
        raw = f.read().decode("utf-8")
    return raw, ("\r\n" if "\r\n" in raw else "\n")


def write(p, text):
    shutil.copyfile(p, p + ".bak")
    with open(p, "wb") as f:
        f.write(text.encode("utf-8"))


def fail(msg):
    sys.exit("ERROR: " + msg)


for p in (mc, pt):
    if not os.path.isfile(p):
        fail("not found: %s\n(run this from the repo root, or pass the repo path as an argument)" % p)

# 1. mousecam.c
raw, nl = read(mc)
if re.search(r"#define CAM_OFF_V143 0x0*71986cull", raw):
    print("mousecam.c: already set")
else:
    new, n = re.subn(r"(#define CAM_OFF_V143 )0ull", r"\g<1>0x0071986cull", raw)
    if n != 1:
        fail("mousecam.c: could not find '#define CAM_OFF_V143 0ull' (changed already? edit it by hand)")
    write(mc, new)
    print("mousecam.c: CAM_OFF_V143 set to 0x0071986c")

# 2. patches-v143.txt
raw, nl = read(pt)
if "set mousecam" in raw or "site mousecam" in raw:
    print("patches-v143.txt: mousecam set already present")
else:
    lines = raw.split(nl)
    idx = next((i for i, l in enumerate(lines) if l.startswith("# Spin waits")), None)
    if idx is None:
        fail("patches-v143.txt: could not find the '# Spin waits' comment to insert before")
    write(pt, nl.join(lines[:idx] + PATCH_LINES + lines[idx:]))
    print("patches-v143.txt: added the mousecam patch set (2 sites)")

print("done - now rebuild")
