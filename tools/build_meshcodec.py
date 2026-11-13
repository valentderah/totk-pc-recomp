"""Build the static library the blueprint preview decodes models with: build/<version>/libmeshcodec.a.

The preview draws each part's real model, and a .bfres.mc is compressed with MeshCodec. The only implementation of it is
dt-12345/MeshCodec, which is GPL-2.0, so none of it is in this repository: this script fetches it at a pinned commit,
compiles it together with this repository's own C door (tools/meshcodec_bridge.cpp), and writes one static library beside
the executable. tools/build_game.py links that library when it is present and defines TOTK_MESHCODEC; a build without it
still works, with a preview that falls back to collision bodies. THIRD_PARTY_NOTICES.md records what distributing a build
that links it means.

Two things about the build are worth knowing before changing it:

* MeshCodec calls zstd's INTERNAL block API (ZSTD_decompressBlock, ZSTD_insertBlock, and it reads dctx->entropy.rep), so it
  needs zstd's internal headers. It does not need zstd's sources: the symbols it calls are exported by the libzstd this
  port already links, so compiling zstd here would only duplicate them and clash at link time. Only the headers are used.
* MeshCodec is C++20, needs lib/float16 for the MSVC path only, and its zstd helper sits in a git submodule, which has to
  be present before its headers exist.

Usage:
    python tools\\build_meshcodec.py --version v143
    python tools\\build_meshcodec.py --version v143 --meshcodec C:\\src\\MeshCodec

Without --meshcodec the checkout is fetched into build/meshcodec/MeshCodec at MESHCODEC_COMMIT, and reused when it is
already there.
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

MESHCODEC_REPO = "https://github.com/dt-12345/MeshCodec"
MESHCODEC_COMMIT = "b3600954872dbc2893a1bcc9e851c1e364f303ab"

# The decoder's own translation units. Everything else in src/ is a header or belongs to a tool the library does not need.
SOURCES = [
    "mc_AttributeCodec.cpp",
    "mc_AttributeStreamInfo.cpp",
    "mc_Codec.cpp",
    "mc_Float.cpp",
    "mc_IndexCodec.cpp",
    "mc_IndexDecompressor.cpp",
    "mc_IndexStreamContext.cpp",
    "mc_StackAllocator.cpp",
    "mc_VertexCodec.cpp",
    "mc_VertexDecompContext.cpp",
    "mc_VertexDecompressor.cpp",
    "mc_Zstd.cpp",
    "mc_MeshCodec.cpp",
]


def tool(name: str) -> Path:
    """A compiler or archiver: the MSYS2 UCRT64 one this port builds with, else whatever is on PATH."""
    ucr = Path("C:/msys64/ucrt64/bin")
    for cand in (ucr / name, ucr / (name + ".exe")):
        if cand.is_file():
            return cand
    found = shutil.which(name)
    if not found:
        sys.exit("build_meshcodec: %s not found (install the MSYS2 UCRT64 toolchain of docs/building.md)" % name)
    return Path(found)


def checkout(where: Path, explicit) -> Path:
    """The MeshCodec source tree, fetched at MESHCODEC_COMMIT when it is not supplied."""
    if explicit:
        mc = Path(explicit)
        if not (mc / "src" / "mc_MeshCodec.cpp").is_file():
            sys.exit("build_meshcodec: %s is not a MeshCodec checkout (no src/mc_MeshCodec.cpp)" % mc)
    else:
        mc = where
        if not (mc / "src" / "mc_MeshCodec.cpp").is_file():
            print("build_meshcodec: fetching MeshCodec at %s into %s" % (MESHCODEC_COMMIT[:12], mc))
            mc.parent.mkdir(parents=True, exist_ok=True)
            git = tool("git")
            subprocess.run([str(git), "clone", "--quiet", MESHCODEC_REPO, str(mc)], check=True)
            subprocess.run([str(git), "-C", str(mc), "checkout", "--quiet", MESHCODEC_COMMIT], check=True)
    # The zstd headers MeshCodec includes live in a submodule; without it nothing compiles.
    zstd = mc / "lib" / "zstd" / "lib"
    if not (zstd / "decompress" / "zstd_decompress_block.h").is_file():
        print("build_meshcodec: initialising the zstd submodule")
        git = tool("git")
        subprocess.run([str(git), "-C", str(mc), "submodule", "update", "--init", "--depth", "1", "lib/zstd"], check=True)
    if not (zstd / "decompress" / "zstd_decompress_block.h").is_file():
        sys.exit("build_meshcodec: %s still has no zstd headers after the submodule update" % mc)
    return mc


def main() -> int:
    ap = argparse.ArgumentParser(description="Build build/<version>/libmeshcodec.a, the preview's model decoder.")
    ap.add_argument("--version", default="v143", help="build version the library is written beside (default v143)")
    ap.add_argument("--meshcodec", help="an existing MeshCodec checkout to use instead of fetching one")
    ap.add_argument("--out", help="write the library here instead of build/<version>/libmeshcodec.a")
    ap.add_argument("--keep-objects", action="store_true", help="leave the intermediate objects beside the library")
    args = ap.parse_args()

    out_dir = ROOT / "build" / args.version
    lib = Path(args.out) if args.out else out_dir / "libmeshcodec.a"
    lib.parent.mkdir(parents=True, exist_ok=True)

    mc = checkout(ROOT / "build" / "meshcodec" / "MeshCodec", args.meshcodec)
    zstd = mc / "lib" / "zstd" / "lib"
    clangxx = tool("clang++")
    ar = tool("ar")

    # MeshCodec's own sources, plus this repository's door. The include order matters: its headers refer to "zstd.h" and
    # to the internal block header, so the zstd tree has to be on the path as if it were a system include directory.
    includes = [
        "-I", str(mc / "src" / "include"),
        "-I", str(mc / "src"),
        "-I", str(zstd),
        "-I", str(zstd / "common"),
        "-I", str(zstd / "decompress"),
        "-I", str(mc / "lib" / "float16"),
    ]
    units = [mc / "src" / s for s in SOURCES] + [ROOT / "tools" / "meshcodec_bridge.cpp"]

    obj_dir = lib.parent / "meshcodec-obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    objs = []
    for src in units:
        if not src.is_file():
            sys.exit("build_meshcodec: missing source %s" % src)
        obj = obj_dir / (src.stem + ".o")
        cmd = [str(clangxx), "-std=c++20", "-O2", "-fno-exceptions", "-fno-rtti", *includes, "-c", str(src), "-o", str(obj)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr[-3000:])
            sys.exit("build_meshcodec: failed on %s" % src.name)
        objs.append(obj)

    if lib.exists():
        lib.unlink()   # ar's 'r' is incremental and would keep members this build no longer has
    r = subprocess.run([str(ar), "rcs", str(lib), *map(str, objs)], capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr)
        sys.exit("build_meshcodec: ar failed")
    if not args.keep_objects:
        shutil.rmtree(obj_dir, ignore_errors=True)

    size = lib.stat().st_size
    print("build_meshcodec: wrote %s (%d sources, %.0f KiB)" % (lib.relative_to(ROOT), len(objs), size / 1024))
    print("build_meshcodec: tools/build_game.py links it when it is present; without it the preview uses collision bodies")
    return 0


if __name__ == "__main__":
    sys.exit(main())