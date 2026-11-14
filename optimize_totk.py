#!/usr/bin/env python3
"""Zelda: Tears of the Kingdom (PC Recomp) - Universal Hardware Optimizer.

Cross-platform tool supporting Windows, Linux (including Steam Deck / SteamOS), and macOS.
Scans system hardware, selects an optimal graphics profile, and configures totk.ini cleanly
without invasive OS modifications.
"""

import os
import sys
import platform
import subprocess
import re
import shutil
from pathlib import Path

# ANSI colors
if sys.platform == "win32":
    os.system("")
CYAN = "\033[96m"
GREEN = "\033[92m"
YELLOW = "\033[93m"
GRAY = "\033[90m"
RED = "\033[91m"
BOLD = "\033[1m"
RESET = "\033[0m"


def print_header():
    print(f"{CYAN}{'=' * 70}{RESET}")
    print(f"{CYAN}{BOLD}       Zelda: Tears of the Kingdom - Hardware Optimizer             {RESET}")
    print(f"{CYAN}{'=' * 70}{RESET}\n")


def find_game_dir():
    candidates = [
        Path.cwd(),
        Path(__file__).resolve().parent,
        Path("C:/Games/TotK") if sys.platform == "win32" else Path.home() / "Games/TotK",
    ]
    for d in candidates:
        if (d / "totk.exe").exists() or (d / "totk").exists() or (d / "totk.ini").exists():
            return d
    return Path.cwd()


def get_windows_memory_gb():
    try:
        import ctypes
        class MEMORYSTATUSEX(ctypes.Structure):
            _fields_ = [
                ("dwLength", ctypes.c_ulong),
                ("dwMemoryLoad", ctypes.c_ulong),
                ("ullTotalPhys", ctypes.c_ulonglong),
                ("ullAvailPhys", ctypes.c_ulonglong),
                ("ullTotalPageFile", ctypes.c_ulonglong),
                ("ullAvailPageFile", ctypes.c_ulonglong),
                ("ullTotalVirtual", ctypes.c_ulonglong),
                ("ullAvailVirtual", ctypes.c_ulonglong),
                ("sullAvailExtendedVirtual", ctypes.c_ulonglong),
            ]
        stat = MEMORYSTATUSEX()
        stat.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(stat))
        return round(stat.ullTotalPhys / (1024 ** 3), 1)
    except Exception:
        return 16.0


def get_screen_resolution():
    system = platform.system()
    if system == "Windows":
        try:
            import ctypes
            user32 = ctypes.windll.user32
            user32.SetProcessDPIAware()
            w = user32.GetSystemMetrics(0)
            h = user32.GetSystemMetrics(1)
            if w > 0 and h > 0:
                return w, h
        except Exception:
            pass
    elif system == "Linux":
        try:
            out = subprocess.check_output(["xrandr"], text=True, stderr=subprocess.DEVNULL)
            m = re.search(r"current\s+(\d+)\s*x\s*(\d+)", out)
            if m:
                return int(m.group(1)), int(m.group(2))
        except Exception:
            pass
    elif system == "Darwin":
        try:
            out = subprocess.check_output(["system_profiler", "SPDisplaysDataType"], text=True, stderr=subprocess.DEVNULL)
            m = re.search(r"Resolution:\s*(\d+)\s*x\s*(\d+)", out)
            if m:
                return int(m.group(1)), int(m.group(2))
        except Exception:
            pass
    return 1920, 1080


def detect_hardware():
    system = platform.system()
    info = {
        "os": f"{system} {platform.release()}",
        "cpu_name": platform.processor() or "Generic CPU",
        "cpu_cores": os.cpu_count() or 4,
        "ram_gb": 16.0,
        "gpu_name": "Standard Vulkan GPU",
        "is_dedicated_gpu": False,
        "screen_w": 1920,
        "screen_h": 1080,
        "is_steam_deck": False,
    }

    # Check Steam Deck
    if system == "Linux" and (os.path.exists("/etc/valve") or os.environ.get("SteamDeck") == "1"):
        info["is_steam_deck"] = True

    w, h = get_screen_resolution()
    info["screen_w"] = w
    info["screen_h"] = h

    if system == "Windows":
        info["ram_gb"] = get_windows_memory_gb()
        try:
            cmd = "powershell -NoProfile -Command \"Get-CimInstance Win32_VideoController | Select-Object -ExpandProperty Name\""
            out = subprocess.check_output(cmd, shell=True, text=True, stderr=subprocess.DEVNULL)
            names = [line.strip() for line in out.strip().splitlines() if line.strip()]
            for name in names:
                if re.search(r"RTX|GTX|Radeon RX|Arc|GeForce", name, re.I) and not re.search(r"Integrated|UHD|Iris|Graphics$", name, re.I):
                    info["gpu_name"] = name
                    info["is_dedicated_gpu"] = True
                    break
            if not info["is_dedicated_gpu"] and names:
                info["gpu_name"] = names[0]
        except Exception:
            pass

        try:
            cmd = "powershell -NoProfile -Command \"(Get-CimInstance Win32_Processor).Name\""
            cpu_name = subprocess.check_output(cmd, shell=True, text=True, stderr=subprocess.DEVNULL).strip()
            if cpu_name:
                info["cpu_name"] = cpu_name
        except Exception:
            pass

    elif system == "Linux":
        try:
            with open("/proc/meminfo", "r") as f:
                for line in f:
                    if "MemTotal" in line:
                        kb = int(line.split()[1])
                        info["ram_gb"] = round(kb / (1024 * 1024), 1)
                        break
        except Exception:
            pass

        try:
            out = subprocess.check_output(["lspci"], text=True, stderr=subprocess.DEVNULL)
            for line in out.splitlines():
                if "VGA" in line or "3D" in line:
                    if any(x in line for x in ["NVIDIA", "Navi", "Radeon RX", "Arc"]):
                        info["gpu_name"] = line.split(":")[-1].strip()
                        info["is_dedicated_gpu"] = True
                        break
                    elif not info["is_dedicated_gpu"]:
                        info["gpu_name"] = line.split(":")[-1].strip()
        except Exception:
            pass

    elif system == "Darwin":
        try:
            out = subprocess.check_output(["sysctl", "-n", "hw.memsize"], text=True, stderr=subprocess.DEVNULL)
            info["ram_gb"] = round(int(out.strip()) / (1024 ** 3), 1)
        except Exception:
            pass
        try:
            out = subprocess.check_output(["sysctl", "-n", "machdep.cpu.brand_string"], text=True, stderr=subprocess.DEVNULL)
            info["cpu_name"] = out.strip()
        except Exception:
            pass
        info["gpu_name"] = "Apple Silicon / Metal"
        info["is_dedicated_gpu"] = True

    return info


def select_performance_profile(hw):
    if hw["is_steam_deck"]:
        return {
            "tier": "Steam Deck Handheld",
            "render_scale": "game",
            "aniso": "game",
            "shadow_scale": 1,
            "fps_limit": 40,
            "upscaler": "bilinear",
            "sharpness": 0,
            "color_preset": "default",
            "fov": 100,
        }
    elif hw["is_dedicated_gpu"]:
        name = hw["gpu_name"]
        high_tier = bool(re.search(r"40[6-9]0|30[7-9]0|2080|6800|6900|7700|7800|7900|A770|Max|Ultra", name, re.I)) or hw["cpu_cores"] >= 8
        if high_tier:
            return {
                "tier": "High-End / Enthusiast",
                "render_scale": "133",
                "aniso": 16,
                "shadow_scale": 2,
                "fps_limit": 90,
                "upscaler": "bilinear",
                "sharpness": 0,
                "color_preset": "default",
                "fov": 110,
            }
        else:
            return {
                "tier": "Balanced / Mid-Range",
                "render_scale": "100",
                "aniso": 16,
                "shadow_scale": 2,
                "fps_limit": 60,
                "upscaler": "bilinear",
                "sharpness": 0,
                "color_preset": "default",
                "fov": 100,
            }
    else:
        # Handheld / Integrated APU (Intel Iris Xe / UHD, AMD Vega APU)
        # Using native 'game' (5/6) render scale and default FOV avoids draw-overhead on low-power APUs
        return {
            "tier": "Handheld / Integrated APU",
            "render_scale": "game",
            "aniso": "game",
            "shadow_scale": 1,
            "fps_limit": 60,
            "upscaler": "bilinear",
            "sharpness": 0,
            "color_preset": "default",
            "fov": 100,
        }


def update_totk_ini(game_dir, hw, prof):
    ini_path = game_dir / "totk.ini"
    if not ini_path.exists():
        example_ini = game_dir / "totk.ini.example"
        if example_ini.exists():
            shutil.copy2(example_ini, ini_path)
        else:
            ini_path.write_text("# totk.ini generated by optimize_totk.py\n[display]\n", encoding="utf-8")

    text = ini_path.read_text(encoding="utf-8", errors="replace")

    updates = {
        "output_res": f"{hw['screen_w']}x{hw['screen_h']}",
        "window_size": f"{hw['screen_w']}x{hw['screen_h']}",
        "fullscreen": "1",
        "aspect": "fit",
        "render_scale": str(prof["render_scale"]),
        "upscaler": prof["upscaler"],
        "sharpness": str(prof["sharpness"]),
        "color_preset": prof["color_preset"],
        "fps_limit": str(prof["fps_limit"]),
        "realtime": "1",
        "cloth_realtime": "1",
        "highfps_fix": "1",
        "aniso": str(prof["aniso"]),
        "shadow_scale": str(prof["shadow_scale"]),
        "contact_shadows": "0",
        "pcss": "0",
        "stick_sens": "3.0",
        "mouse_sens": "1.0",
        "fov": str(prof["fov"]),
    }

    # Only set gpu_name if an NVIDIA discrete GPU was specifically identified
    is_nvidia = hw["is_dedicated_gpu"] and bool(re.search(r"NVIDIA|GeForce|RTX|GTX", hw["gpu_name"], re.I))
    if is_nvidia:
        updates["gpu_name"] = "NVIDIA"
    else:
        text = re.sub(r"(?m)^gpu_name\s*=.*", "# gpu_name = NVIDIA", text)

    for key, val in updates.items():
        pattern = rf"(?m)^#?\s*{re.escape(key)}\s*=.*"
        if re.search(pattern, text):
            text = re.sub(pattern, f"{key} = {val}", text)
        else:
            text += f"\n{key} = {val}"

    ini_path.write_text(text, encoding="utf-8")
    print(f"      {GREEN}[OK] totk.ini successfully configured!{RESET}")


def main():
    print_header()
    game_dir = find_game_dir()
    print(f"{GRAY}[*] Target Game Directory: {game_dir}{RESET}\n")

    # 1. Scan Hardware
    print(f"{CYAN}[1/3] Scanning System Hardware...{RESET}")
    hw = detect_hardware()
    print(f"      OS      : {hw['os']} {'(Steam Deck)' if hw['is_steam_deck'] else ''}")
    print(f"      CPU     : {hw['cpu_name']} ({hw['cpu_cores']} Cores)")
    print(f"      RAM     : {hw['ram_gb']} GB")
    print(f"      GPU     : {hw['gpu_name']}")
    print(f"      Display : {hw['screen_w']}x{hw['screen_h']}\n")

    # 2. Profile Selection
    print(f"{CYAN}[2/3] Evaluating Best Performance Profile...{RESET}")
    prof = select_performance_profile(hw)
    print(f"      Selected Tier : {GREEN}{prof['tier']}{RESET}")
    print(f"      Configuration : RenderScale={prof['render_scale']}, Aniso={prof['aniso']}, Shadows={prof['shadow_scale']}, FPS={prof['fps_limit']}, FOV={prof['fov']}\n")

    # 3. Configure INI
    print(f"{CYAN}[3/3] Updating totk.ini Settings...{RESET}")
    update_totk_ini(game_dir, hw, prof)
    print()

    print(f"{GREEN}{'=' * 70}{RESET}")
    print(f"{GREEN}{BOLD}                    OPTIMIZATION COMPLETE!                            {RESET}")
    print(f"{GREEN}{'=' * 70}{RESET}\n")


if __name__ == "__main__":
    main()
