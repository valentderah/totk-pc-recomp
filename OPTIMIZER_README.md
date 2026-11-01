# TotK PC Optimizer & Fluid Mouse Look Fix

An automated, smart hardware-detecting optimization tool, binary patcher, and laptop GPU display switcher for *The Legend of Zelda: Tears of the Kingdom* PC Recompilation.

---

## 🌟 What This Tool Does

### 1. 🖱️ True PC Fluid Mouse Camera Fix
In the native PC recompilation, mouse movement is translated into virtual right-analog thumbstick deflection. However, the default implementation suffers from two major issues:
* **The 33% Speed Ceiling Throttle (`vdivpd` bug):** The input polling loop (`poll_merged`) intentionally divides mouse stick deflection by the camera speed multiplier (3.0×). This permanently limits mouse movement to a maximum of **33% deflection**, making fast turns physically impossible regardless of how fast you swipe.
* **The 3000.0 Dead-Zone Divisor:** Mouse velocity is divided by a static `3000.0` constant, meaning normal wrist and desk movements only register as a 5%–10% stick nudge (feeling like dragging through wet cement).

**The Solution:**
This tool automatically patches `totk.exe`:
* Replaces the `vdivpd` instruction with `vmovapd`, removing the 33% speed throttle entirely.
* Calibrates the velocity divisor from `3000.0` down to `1800.0` for natural, fluid 1:1 camera tracking matching third-person PC titles like *GTA* or *The Witcher 3*.
* Safely creates a `totk.exe.bak` backup before applying any modifications.

---

### 2. 🖥️ Automatic Hardware Detection & Profile Tuning
The script inspects your system hardware in real time:
* **Display Resolution & Aspect Ratio:** Identifies 16:9, 16:10, 21:9, and 32:9 displays and configures `output_res` and `window_size` to your native resolution, completely eliminating black bars and distorted stretching.
* **GPU & CPU Classification:** Categorizes your system into one of three tailored performance profiles:

| Hardware Tier | Qualifications | Resolution & Scale | Aniso | Shadows | FPS Target | Post-Processing |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **High-End / Enthusiast** | Dedicated GPU (8 GB+ VRAM, RTX 40-series/30-series, RX 7000) & 8+ cores | Native Res @ **133% Supersampling (SSAA)** | 16× | 2× Resolution | **90 FPS** (Real-time physics) | Clarity preset, Bilinear |
| **Balanced / Mid-Range** | Dedicated GPU (4–6 GB VRAM, GTX 1660, RTX 3050, RX 6600) | Native Res @ **100% 1:1 Scale** | 16× | 2× Resolution | **60 FPS** | Default preset, Bilinear |
| **Handheld / Low-Power APU** | Integrated Graphics (Intel Iris Xe, AMD Vega/RDNA, Steam Deck) | Native Res @ **85% Scale** | 4× | 1× Resolution | **40 FPS / 30 FPS** | FSR 1 (20% Sharpness) |

---

### 3. 🔋 Laptop GPU & Display Switching (NVIDIA Advanced Optimus / MUX Fix)
On gaming laptops equipped with dual GPUs (e.g. Intel UHD / AMD Radeon iGPU + NVIDIA GeForce RTX dGPU) and Advanced Optimus (Dynamic Display Switching / DDS):
* **Why Displays Get Stuck on Dedicated GPU:** When a game launches, the internal MUX hardware flips the laptop screen directly to the NVIDIA dGPU. While the display is connected to the dGPU, background Windows desktop processes (`SearchHost.exe`, `StartMenuExperienceHost.exe`, `TextInputHost.exe`, `ShellHost.exe`, etc.) initialize their DirectX swapchains on the dGPU.
* **The Problem:** When the game closes, NVIDIA Advanced Optimus refuses to switch the display back to the energy-efficient integrated GPU because those background shell apps are still holding open handles on the dGPU. This drains battery life and keeps the laptop running warm.
* **Hardware MUX Locking:** If the laptop is in ASUS Armoury Crate **"Ultimate"** mode or NVIDIA Control Panel **"NVIDIA GPU only"**, the internal display is permanently locked to the dGPU and cannot dynamically switch back until changed to **"Standard"** or **"Automatic Select"**.

**The Solution Built Into This Tool:**
1. **Windows Graphics Preference:** Automatically registers `totk.exe` in Windows Graphics Settings (`HKCU:\Software\Microsoft\DirectX\UserGpuPreferences`) with `GpuPreference=2;` (High Performance dGPU) while keeping Windows shell processes on Power Saving (iGPU).
2. **MUX Status Diagnostics:** Checks your system's MUX persistence registry to detect whether your laptop is currently locked to "NVIDIA GPU Only" or in "Automatic Select" mode.
3. **Automated dGPU Release & Session Wrapper:** Closes sticky background processes holding onto the dGPU, letting Windows instantly restart them on the integrated GPU so the laptop display smoothly switches back to the iGPU.
4. **Standalone Quick-Switch Utility:** Includes `Switch_To_iGPU.bat` for one-click manual release anytime.

---

## 🚀 How to Use

### Method 1: The All-in-One Optimizer & Launcher
1. Double-click **`Optimize_TotK_PC.bat`**.
2. The tool will:
   * Close any running game instances.
   * Scan your hardware specs and select the optimal profile.
   * Patch `totk.exe` with fluid mouse camera controls (with backup).
   * Write optimal resolution, frame rate, and graphic settings to `totk.ini`.
   * Register high-performance GPU preferences and verify laptop display MUX state.
3. Press **`Y`** to launch the game. When you exit the game, the script will automatically clean up background apps and release the display back to your integrated GPU!

### Method 2: Manual GPU Switcher (Laptops)
If your laptop display ever remains on the dedicated GPU after playing:
* Double-click **`Switch_To_iGPU.bat`**. It will instantly terminate the sticky background processes and allow Advanced Optimus to switch your display back to the Intel / AMD integrated GPU.

---

## ⚙️ Recommended Laptop Settings for Seamless GPU Switching

* **ASUS Laptops (Armoury Crate / G-Helper):**
  * Open Armoury Crate and set **GPU Mode** to **Standard** (or **Optimized**). Avoid leaving it on **Ultimate** if you want your laptop display to switch back to the iGPU when not gaming.
* **NVIDIA Control Panel:**
  * Right-click the desktop → **NVIDIA Control Panel** → **Manage Display Mode**.
  * Select **Automatic Select** (for Dynamic Display Switching) or **Optimus** (integrated GPU handles desktop display; dGPU handles game rendering).
