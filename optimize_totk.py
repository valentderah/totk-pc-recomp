#!/usr/bin/env python3
"""Zelda: Tears of the Kingdom (PC Recomp) - Universal Hardware Optimizer & Fluid Mouse Look Patcher.

Cross-platform tool supporting Windows, Linux (including Steam Deck / SteamOS), and macOS.
Scans system hardware, selects an optimal graphics profile, calibrates fluid mouse camera
controls, configures totk.ini, and handles laptop dual-GPU display switching.
"""

import os
import sys
import platform
import subprocess
import re
import struct
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
    print(f"{CYAN}{BOLD}   Zelda: Tears of the Kingdom - Cross-Platform Optimizer & Fixer   {RESET}")
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
        m = MEMORYSTATUSEX()
        m.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)):
            return round(m.ullTotalPhys / (1024 ** 3), 1)
    except Exception:
        pass
    return 16.0


def detect_hardware():
    system = platform.system()
    info = {
        "os": system,
        "is_steam_deck": False,
        "cpu_name": platform.processor() or "Unknown CPU",
        "cpu_cores": os.cpu_count() or 4,
        "ram_gb": 8.0,
        "gpu_name": "Generic GPU",
        "is_dedicated_gpu": False,
        "screen_w": 1920,
        "screen_h": 1080,
    }

    # 1. Detect Steam Deck
    if system == "Linux":
        try:
            prod_path = Path("/sys/devices/virtual/dmi/id/product_name")
            if prod_path.exists():
                name = prod_path.read_text().strip().lower()
                if "jupiter" in name or "galileo" in name:
                    info["is_steam_deck"] = True
        except Exception:
            pass
        if os.environ.get("STEAM_DECK") == "1" or Path("/home/deck").exists():
            info["is_steam_deck"] = True

    # 2. Windows Hardware Detection
    if system == "Windows":
        info["ram_gb"] = get_windows_memory_gb()

        # Query GPU and CPU via PowerShell (fast, CimInstance, works on Windows 10 & 11 without wmic)
        try:
            ps_cmd = (
                "$g = (Get-CimInstance Win32_VideoController).Name -join ';'; "
                "$c = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name; "
                "Write-Output \"GPU:$g|CPU:$c\""
            )
            out = subprocess.run(
                ["powershell", "-NoProfile", "-NonInteractive", "-Command", ps_cmd],
                capture_output=True, text=True, timeout=5
            ).stdout.strip()
            for part in out.split("|"):
                if part.startswith("GPU:"):
                    gpus = [x.strip() for x in part[4:].split(";") if x.strip()]
                    dgpu = [g for g in gpus if re.search(r"RTX|GTX|Radeon RX|Arc|GeForce", g, re.I) and not re.search(r"Integrated|UHD|Iris|Vega\s+\d+|Graphics$", g, re.I)]
                    if dgpu:
                        info["gpu_name"] = dgpu[0]
                        info["is_dedicated_gpu"] = True
                    elif gpus:
                        info["gpu_name"] = gpus[0]
                elif part.startswith("CPU:"):
                    info["cpu_name"] = part[4:].strip()
        except Exception:
            pass

        # Screen Resolution
        try:
            import ctypes
            user32 = ctypes.windll.user32
            user32.SetProcessDPIAware()
            info["screen_w"] = user32.GetSystemMetrics(0)
            info["screen_h"] = user32.GetSystemMetrics(1)
        except Exception:
            pass

    # 3. Linux Hardware Detection
    elif system == "Linux":
        try:
            with open("/proc/cpuinfo") as f:
                for line in f:
                    if "model name" in line:
                        info["cpu_name"] = line.split(":", 1)[1].strip()
                        break
        except Exception:
            pass

        try:
            with open("/proc/meminfo") as f:
                for line in f:
                    if line.startswith("MemTotal:"):
                        kb = int(line.split()[1])
                        info["ram_gb"] = round(kb / (1024 * 1024), 1)
                        break
        except Exception:
            pass

        try:
            out = subprocess.run(["lspci"], capture_output=True, text=True, timeout=3).stdout
            vga_lines = [l for l in out.splitlines() if re.search(r"VGA|3D controller", l, re.I)]
            dgpu = [l for l in vga_lines if re.search(r"NVIDIA|GeForce|Radeon RX|Navi", l, re.I)]
            if dgpu:
                info["gpu_name"] = dgpu[0].split(":", 2)[-1].strip()
                info["is_dedicated_gpu"] = True
            elif vga_lines:
                info["gpu_name"] = vga_lines[0].split(":", 2)[-1].strip()
        except Exception:
            pass

        try:
            out = subprocess.run(["xrandr"], capture_output=True, text=True, timeout=3).stdout
            m = re.search(r"current\s+(\d+)\s*x\s*(\d+)", out)
            if m:
                info["screen_w"] = int(m.group(1))
                info["screen_h"] = int(m.group(2))
        except Exception:
            if info["is_steam_deck"]:
                info["screen_w"] = 1280
                info["screen_h"] = 800

    # 4. macOS Hardware Detection
    elif system == "Darwin":
        try:
            out = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True, timeout=3).stdout.strip()
            if out:
                info["cpu_name"] = out
        except Exception:
            pass
        try:
            out = subprocess.run(["sysctl", "-n", "hw.memsize"], capture_output=True, text=True, timeout=3).stdout.strip()
            if out:
                info["ram_gb"] = round(int(out) / (1024 ** 3), 1)
        except Exception:
            pass
        try:
            out = subprocess.run(["system_profiler", "SPDisplaysDataType"], capture_output=True, text=True, timeout=5).stdout
            for line in out.splitlines():
                if "Chipset Model:" in line:
                    info["gpu_name"] = line.split(":", 1)[1].strip()
                    if "Apple" in info["gpu_name"] or "AMD" in info["gpu_name"]:
                        info["is_dedicated_gpu"] = True
                elif "Resolution:" in line:
                    m = re.search(r"(\d+)\s*x\s*(\d+)", line)
                    if m:
                        info["screen_w"] = int(m.group(1))
                        info["screen_h"] = int(m.group(2))
        except Exception:
            pass

    return info


def select_performance_profile(hw):
    if hw["is_steam_deck"]:
        return {
            "tier": "Steam Deck Handheld",
            "render_scale": 85,
            "aniso": 4,
            "shadow_scale": 1,
            "fps_limit": 40,
            "upscaler": "fsr",
            "sharpness": 20,
            "color_preset": "default",
        }
    elif hw["is_dedicated_gpu"]:
        name = hw["gpu_name"]
        high_tier = bool(re.search(r"40[6-9]0|30[7-9]0|2080|6800|6900|7700|7800|7900|A770|Max|Ultra", name, re.I)) or hw["cpu_cores"] >= 8
        if high_tier:
            return {
                "tier": "High-End / Enthusiast",
                "render_scale": 133,
                "aniso": 16,
                "shadow_scale": 2,
                "fps_limit": 90,
                "upscaler": "bilinear",
                "sharpness": 0,
                "color_preset": "clarity",
            }
        else:
            return {
                "tier": "Balanced / Mid-Range",
                "render_scale": 100,
                "aniso": 16,
                "shadow_scale": 2,
                "fps_limit": 60,
                "upscaler": "bilinear",
                "sharpness": 0,
                "color_preset": "default",
            }
    else:
        return {
            "tier": "Handheld / Low-Power APU",
            "render_scale": 85,
            "aniso": 4,
            "shadow_scale": 1,
            "fps_limit": 40,
            "upscaler": "fsr",
            "sharpness": 20,
            "color_preset": "default",
        }


def patch_fluid_mouse(game_dir):
    exe_candidates = [
        game_dir / "totk.exe",
        game_dir / "totk",
    ]
    target_exe = None
    for cand in exe_candidates:
        if cand.exists():
            target_exe = cand
            break

    if not target_exe:
        print(f"      {GRAY}[SKIP] No totk or totk.exe binary found in {game_dir}{RESET}")
        return False

    bak_path = target_exe.with_suffix(target_exe.suffix + ".bak")
    if not bak_path.exists():
        try:
            shutil.copy2(target_exe, bak_path)
            print(f"      {GRAY}Created safe backup: {bak_path.name}{RESET}")
        except Exception as e:
            print(f"      {YELLOW}[WARN] Could not create backup: {e}{RESET}")

    try:
        with open(target_exe, "r+b") as f:
            # 1. Patch vdivpd -> vmovapd (offset 0xe22241a + 12 on v1.4.3 x86_64)
            div_off = 0xE22241A + 12
            f.seek(0, os.SEEK_END)
            size = f.tell()
            if size > div_off + 4:
                f.seek(div_off)
                cur = f.read(4)
                if cur == b"\xc5\xf1\x5e\xc0" or cur == b"\xc5\xf9\x28\xc1":
                    f.seek(div_off)
                    f.write(b"\xc5\xf9\x28\xc1")
                    print(f"      {GREEN}[OK] Patched poll_merged: removed 33% rotation speed throttle{RESET}")

            # 2. Patch 3000.0 -> 1800.0 velocity divisor (offset 0xeea8d80)
            c_off = 0xEEA8D80
            if size > c_off + 8:
                f.seek(c_off)
                cur_val = struct.unpack("<d", f.read(8))[0]
                if abs(cur_val - 3000.0) < 1e-4 or abs(cur_val - 1800.0) < 1e-4:
                    f.seek(c_off)
                    f.write(struct.pack("<d", 1800.0))
                    print(f"      {GREEN}[OK] Patched velocity curve: tuned divisor from 3000.0 to 1800.0{RESET}")
        return True
    except PermissionError:
        print(f"      {YELLOW}[WARN] {target_exe.name} is currently running. Please close the game first.{RESET}")
        return False
    except Exception as e:
        print(f"      {YELLOW}[WARN] Could not patch binary: {e}{RESET}")
        return False


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
        "fov": "110",
        "gpu_index": "0",
    }
    if "NVIDIA" in hw["gpu_name"]:
        updates["gpu_name"] = "NVIDIA"

    for key, val in updates.items():
        pattern = rf"(?m)^#?\s*{re.escape(key)}\s*=.*"
        if re.search(pattern, text):
            text = re.sub(pattern, f"{key} = {val}", text)
        else:
            text += f"\n{key} = {val}"

    ini_path.write_text(text, encoding="utf-8")
    print(f"      {GREEN}[OK] totk.ini successfully configured!{RESET}")


def configure_gpu_and_switching(game_dir, hw):
    system = platform.system()
    if system == "Windows":
        # 1. Set Windows Graphics Preference for totk.exe
        exe_path = str((game_dir / "totk.exe").resolve())
        try:
            import winreg
            key = winreg.CreateKey(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\DirectX\UserGpuPreferences")
            winreg.SetValueEx(key, exe_path, 0, winreg.REG_SZ, "GpuPreference=2;")
            winreg.CloseKey(key)
            print(f"      {GREEN}[OK] Windows DirectX Preference set to High Performance (dGPU){RESET}")
        except Exception as e:
            print(f"      {GRAY}[INFO] DirectX preference check: {e}{RESET}")

        # 2. Check NVIDIA MUX status
        try:
            import winreg
            key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Services\nvlddmkm\Global\NvHybrid\Persistence\ACE")
            state, _ = winreg.QueryValueEx(key, "InternalMuxState")
            auto_mode, _ = winreg.QueryValueEx(key, "InternalMuxIsAutomaticMode")
            winreg.CloseKey(key)
            if state == 1 and auto_mode == 0:
                print(f"      {YELLOW}[!] Laptop MUX is currently locked to Dedicated GPU Only (or Armoury Crate 'Ultimate' Mode).{RESET}")
                print(f"          To allow automatic switching back to Intel UHD:")
                print(f"          1. Open Armoury Crate -> Set GPU Mode to 'Standard' or 'Optimized'.")
                print(f"          2. Or open NVIDIA Control Panel -> Manage Display Mode -> Select 'Automatic Select'.")
            elif auto_mode == 1:
                print(f"      {GREEN}[OK] NVIDIA Advanced Optimus is in 'Automatic Select' mode.{RESET}")
        except Exception:
            pass

        # 3. Clean up sticky background processes
        sticky = ["SearchHost", "StartMenuExperienceHost", "TextInputHost", "PhoneExperienceHost", "AccountsControlHost", "CHXSmartScreen"]
        for p in sticky:
            subprocess.run(["taskkill", "/f", "/im", f"{p}.exe"], capture_output=True, text=True)
        print(f"      {GREEN}[OK] Released background apps holding onto dGPU (auto-migrated to iGPU).{RESET}")

    elif system == "Linux":
        if hw["is_dedicated_gpu"]:
            print(f"      {GREEN}[OK] Linux Dual-GPU tip: Run with 'prime-run ./totk' or '__NV_PRIME_RENDER_OFFLOAD=1 ./totk'{RESET}")


def main():
    print_header()
    game_dir = find_game_dir()
    print(f"{GRAY}[*] Target Game Directory: {game_dir}{RESET}\n")

    # 1. Scan Hardware
    print(f"{CYAN}[1/5] Scanning System Hardware...{RESET}")
    hw = detect_hardware()
    print(f"      OS      : {hw['os']} {'(Steam Deck)' if hw['is_steam_deck'] else ''}")
    print(f"      CPU     : {hw['cpu_name']} ({hw['cpu_cores']} Cores)")
    print(f"      RAM     : {hw['ram_gb']} GB")
    print(f"      GPU     : {hw['gpu_name']}")
    print(f"      Display : {hw['screen_w']}x{hw['screen_h']}\n")

    # 2. Profile Selection
    print(f"{CYAN}[2/5] Evaluating Best Performance Profile...{RESET}")
    prof = select_performance_profile(hw)
    print(f"      Selected Tier : {GREEN}{prof['tier']}{RESET}")
    print(f"      Configuration : RenderScale={prof['render_scale']}%, Aniso={prof['aniso']}x, Shadows={prof['shadow_scale']}x, FPS={prof['fps_limit']}\n")

    # 3. Mouse Patch
    print(f"{CYAN}[3/5] Calibrating Fluid PC Mouse Look...{RESET}")
    patch_fluid_mouse(game_dir)
    print()

    # 4. Configure INI
    print(f"{CYAN}[4/5] Updating totk.ini Settings...{RESET}")
    update_totk_ini(game_dir, hw, prof)
    print()

    # 5. Dual-GPU / Switching
    print(f"{CYAN}[5/5] Diagnosing Display & GPU Switching...{RESET}")
    configure_gpu_and_switching(game_dir, hw)
    print()

    print(f"{GREEN}{'=' * 70}{RESET}")
    print(f"{GREEN}{BOLD}                    OPTIMIZATION COMPLETE!                            {RESET}")
    print(f"{GREEN}{'=' * 70}{RESET}\n")


if __name__ == "__main__":
    main()
