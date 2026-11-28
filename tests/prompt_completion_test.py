"""Test exact production folder scheduling and prompt readiness without game data.

Fake time and readiness adapters make child completion deterministic. This checks
menu state, throttling and launch requests, not the generator or filesystem API.
"""
import argparse
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang++")
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    source, out = args.source.resolve(), args.out.resolve()
    if not any(out.is_relative_to(source / p) for p in ("build", "generated")):
        parser.error("--out must be under the selected checkout's build/ or generated/")
    if out.exists():
        parser.error("--out must be a new directory")
    text = (source / "runtime/ui/menu.cpp").read_text(encoding="utf-8")
    start = text.index("enum { SETTLE_NS =")
    end = text.index("// The retry under a row", start)
    scheduler = text[start:end]
    start = text.index("static const char *prompts_note(bool *restart)")
    end = text.index('// The confirmation of "Restart now"', start)
    out.mkdir(parents=True)
    worker = "#define HAS_FOLDER_WORKER 1\n" if "folders_check_worker" in scheduler else "#define HAS_FOLDER_WORKER 0\n"
    (out / "prompt_completion_body.h").write_text(worker + scheduler + text[start:end], encoding="utf-8")
    exe = out / "prompt_completion_test.exe"
    subprocess.run([
        args.cc, "-std=c++17", "-pthread", *(["-static"] if sys.platform == "win32" else []),
        "-Wall", "-Wextra", "-Werror", "-I", str(out),
        str(Path(__file__).with_suffix(".cpp")), "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], cwd=out, check=True)


if __name__ == "__main__":
    main()
