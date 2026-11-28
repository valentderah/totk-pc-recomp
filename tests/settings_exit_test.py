"""Close during the menu save debounce using production shutdown/config code.

Compiles the selected checkout's exact gfx_window_closed and option_rows writing
section with full cfg.c/input_map.c. Synthetic platform adapters supply a fake
clock and real files. Child processes take the normal exit/restart shutdown path;
the parent checks disk bytes after exit. No game, GUI or Vulkan is required.
"""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    source, out = args.source.resolve(), args.out.resolve()
    if not any(out.is_relative_to(source / p) for p in ("build", "generated")):
        parser.error("--out must be under the selected checkout's build/ or generated/")
    if out.exists():
        parser.error("--out must be a new directory")
    out.mkdir(parents=True)
    gfx = (source / "runtime/gfx_present.c").read_text(encoding="utf-8")
    start = gfx.index("void gfx_window_closed(void) {")
    end = gfx.index("\nvoid gfx_window_request_restart", start)
    (out / "settings_close_body.h").write_text(gfx[start:end], encoding="utf-8")
    rows = (source / "runtime/ui/option_rows.cpp").read_text(encoding="utf-8")
    start = rows.index("static uint64_t g_save_at;")
    end = rows.index("// ---- rows", start)
    (out / "settings_rows_body.h").write_text(rows[start:end], encoding="utf-8")
    exe = out / "settings_exit_test.exe"
    subprocess.run([
        args.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(source / "runtime"),
        "-I", str(out), str(Path(__file__).with_suffix(".c")), str(source / "runtime/cfg.c"),
        str(source / "runtime/input_map.c"), "-lm", "-o", str(exe),
    ], check=True)
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("TOTK_")}
    env["TOTK_DEVICE_PROFILE"] = "desktop"
    keep = b"# retained comment\r\n[controls]\r\n; bind_a = key:default,pad:default\r\n"
    tail = b"custom_note = untouched\r\n"
    initial = keep + b"volume = 35\r\nbind_a = key:f,pad:west\r\n" + tail
    failures = 0
    checks = 0
    for restart in (0, 1):
        for mode in ("scalar", "reset", "binding", "clean", "failed", "elapsed"):
            case = out / f"{mode}-restart{restart}"
            case.mkdir()
            ini = case / "totk.ini"
            ini.write_bytes(initial)
            child = subprocess.run([str(exe), mode, str(restart)], cwd=case, env=env)
            expected = initial
            if mode in ("scalar", "elapsed"):
                expected = initial.replace(b"volume = 35", b"volume = 62")
            elif mode == "reset":
                expected = keep + b"bind_a = key:f,pad:west\r\n" + tail
            elif mode == "binding":
                expected = initial.replace(b"bind_a = key:f,pad:west", b"bind_a = key:g,pad:north")
            assertions = [
                (child.returncode == 0, "child shutdown assertions"),
                (ini.read_bytes() == expected, "persisted exact bytes"),
                ((case / "restart.marker").exists() == bool(restart), "restart invocation"),
            ]
            if restart:
                assertions.append(((case / "restart.ini").read_bytes() == expected, "saved before restart"))
            for ok, label in assertions:
                checks += 1
                failures += not ok
                print(f"{'PASS' if ok else 'FAIL'} {mode} restart={restart}: {label}")
    print(f"{checks} checks, {failures} failures", flush=True)
    raise SystemExit(bool(failures))


if __name__ == "__main__":
    main()
