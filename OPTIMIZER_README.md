# TotK Universal Hardware Optimizer & Fluid Mouse Look Fix

A universal, cross-platform hardware-detecting optimization tool, binary patcher, and laptop GPU switcher for *The Legend of Zelda: Tears of the Kingdom* PC Recompilation.

**Supported Platforms:** Windows 10/11, Linux (including Steam Deck / SteamOS), and macOS (Apple Silicon & Intel).

---

## 🌟 What This Tool Does

### 1. 🖱️ True PC Fluid Mouse Camera Fix (Universal)
In the native PC recompilation, mouse movement is translated into virtual right-analog thumbstick deflection. However, the default implementation suffers from two major issues:
* **The 33% Speed Ceiling Throttle (`vdivpd` bug):** The input polling loop (`poll_merged`) intentionally divides mouse stick deflection by the camera speed multiplier (3.0×). This permanently limits mouse movement to a maximum of **33% deflection**, making fast turns physically impossible regardless of how fast you swipe.
* **The 3000.0 Dead-Zone Divisor:** Mouse velocity is divided by a static `3000.0` constant, meaning normal wrist and desk movements only register as a 5%–10% stick nudge (feeling like dragging through wet cement).

**The Solution:**
This tool automatically patches `totk.exe` (Windows) or `totk` (Linux / macOS):
* Replaces the `vdivpd` instruction with `vmovapd`, removing the 33% speed throttle entirely.
* Calibrates the velocity divisor from `3000.0` down to `1800.0` for natural, fluid 1:1 camera tracking matching third-person PC titles like *GTA* or *The Witcher 3*.
* Safely creates a `.bak` backup before applying any modifications.

---

### 2. 🖥️ Automatic Hardware Detection & Profile Tuning
The script inspects your system hardware in real time:
* **Display Resolution & Aspect Ratio:** Identifies 16:9, 16:10, 21:9, and 32:9 displays and configures `output_res` and `window_size` to your native resolution, completely eliminating black bars and distorted stretching.
* **GPU & CPU Classification:** Categorizes your system into one of three tailored performance profiles:

| Hardware Tier | Qualifications | Resolution & Scale | Aniso | Shadows | FPS Target | Post-Processing |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **High-End / Enthusiast** | Dedicated GPU (8 GB+ VRAM, RTX 40/30-series, RX 7000, Apple Max/Ultra) & 8+ cores | Native Res @ **133% Supersampling (SSAA)** | 16× | 2× Resolution | **90 FPS** (Real-time physics) | Clarity preset, Bilinear |
| **Balanced / Mid-Range** | Dedicated GPU (4–6 GB VRAM, GTX 1660, RTX 3050, RX 6600, Apple Pro) | Native Res @ **100% 1:1 Scale** | 16× | 2× Resolution | **60 FPS** | Default preset, Bilinear |
| **Handheld / Low-Power APU** | Integrated Graphics (Intel Iris Xe, AMD Vega/RDNA, Steam Deck) | Native Res @ **85% Scale** | 4× | 1× Resolution | **40 FPS / 30 FPS** | FSR 1 (20% Sharpness) |

---

### 3. 🔋 Laptop GPU & Display Switching (NVIDIA Advanced Optimus / MUX Fix)
On gaming laptops equipped with dual GPUs (e.g. Intel UHD / AMD Radeon iGPU + NVIDIA GeForce RTX dGPU) and Advanced Optimus (Dynamic Display Switching / DDS):
* **Why Displays Get Stuck on Dedicated GPU:** When a game launches, the internal MUX hardware flips the laptop screen directly to the NVIDIA dGPU. While the display is connected to the dGPU, background Windows desktop processes (`SearchHost.exe`, `StartMenuExperienceHost.exe`, `TextInputHost.exe`, `ShellHost.exe`, etc.) initialize their DirectX swapchains on the dGPU.
* **The Problem:** When the game closes, NVIDIA Advanced Optimus refuses to switch the display back to the energy-efficient integrated GPU because those background shell apps are still holding open handles on the dGPU. This drains battery life and keeps the laptop running warm.
* **The Fix:** Configures Windows DirectX preferences to keep desktop apps off the dGPU, diagnoses laptop MUX lock, and terminates lingering shell processes so the internal screen immediately reverts to the integrated GPU upon quitting.

---

## 🚀 How to Run

### Windows
Run via terminal or double-click:
```cmd
python optimize_totk.py
```
*(Or double-click `Optimize_TotK_PC.bat`)*

### Linux & Steam Deck
```bash
python3 optimize_totk.py
```

### macOS
```bash
python3 optimize_totk.py
```

*Zero external dependencies required — runs on standard Python 3.8+ out of the box.*
