"""Exercise the menu's actual reset_all body with production cfg/input mapping.

The selected source may be an unchanged checkout to demonstrate the regression.
Only platform I/O/locks and the UI save scheduler are replaced by fixture adapters;
this does not test ImGui rendering, platform atomic writes, or save debounce timing.
"""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    source = args.source.resolve()
    out = args.out.resolve()
    if not any(out.is_relative_to(source / folder) for folder in ("build", "generated")):
        parser.error("--out must be under the selected source's build/ or generated/")
    if out.exists():
        parser.error("--out must be a new directory")

    menu = (source / "runtime/ui/menu.cpp").read_text(encoding="utf-8")
    start = menu.index("static void reset_all(void) {")
    end = menu.index("// A button's result", start)
    body = menu[start:end].strip()
    if not body.endswith("}"):
        raise RuntimeError("could not isolate menu reset_all")
    out.mkdir(parents=True)
    (out / "menu_reset_body.h").write_text(body + "\n", encoding="utf-8")
    executable = out / "menu_reset_bindings_test.exe"
    subprocess.run([
        args.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
        "-I", str(source / "runtime"), "-I", str(out),
        str(Path(__file__).with_suffix(".c")), str(source / "runtime/cfg.c"),
        str(source / "runtime/input_map.c"), "-lm", "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], cwd=out, check=True)


if __name__ == "__main__":
    main()
