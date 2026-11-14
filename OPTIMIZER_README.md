# TotK Hardware Optimizer & Native PC Mouse Look Improvement

A clean, non-invasive hardware-detecting optimization tool and native mouse look improvement for *The Legend of Zelda: Tears of the Kingdom* PC Recompilation.

**Supported Platforms:** Windows 10/11, Linux (including Steam Deck / SteamOS), and macOS (Apple Silicon & Intel).

---

## 🌟 What This Includes

### 1. 🖱️ Native Fluid Mouse Camera Controls (`runtime/input.c` & `runtime/input_map.c`)
In third-person PC titles, mouse camera tracking should feel direct, responsive, and 1:1. Two key improvements have been made natively to the runtime:
* **Uncapped Stick Deflection in `runtime/input.c`:** Previously in `mouse_camera()`, the mouse stick deflection sample was divided by `cam_speed_applied()` (the camera speed factor, e.g. 3.0×). This created an artificial speed ceiling capping maximum mouse stick deflection at only 33%, causing fast mouse swipes to feel throttled. Full 100% virtual stick deflection is now preserved.
* **Calibrated Velocity Divisor in `runtime/input_map.c`:** `MOUSE_FULL_CPS` is calibrated from `3000.0` down to `1800.0`, eliminating dead-zone sluggishness during small wrist/micro-aiming movements while preserving smooth high-speed rotation.

---

### 2. 🖥️ Hardware-Based Configuration (`optimize_totk.py` / `Optimize_TotK_PC.ps1`)
The optimizer detects system hardware in real time and automatically writes optimal settings to `totk.ini`:
* **Display Resolution & Aspect Ratio:** Auto-detects 16:9, 16:10, 21:9, and 32:9 screens and matches `output_res` and `window_size` to prevent black bars and aspect stretching.
* **GPU Detection & Selection:** Accurately identifies discrete vs. integrated graphics. Only sets `gpu_name = NVIDIA` when an NVIDIA GPU is confirmed present, ensuring systems with Intel Iris Xe or AMD graphics default to standard Vulkan device enumeration without crashes.
* **Tailored Performance Tiers:**

| Hardware Tier | Qualifications | Resolution & Scale | Aniso | Shadows | FPS Target | FOV |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **High-End / Enthusiast** | Dedicated GPU (8 GB+ VRAM, RTX 40/30-series, RX 7000, Apple Max/Ultra) & 8+ cores | Native Res @ **133% Supersampling (SSAA)** | 16× | 2× Resolution | **90 FPS** (Real-time physics) | 110 |
| **Balanced / Mid-Range** | Dedicated GPU (4–6 GB VRAM, GTX 1660, RTX 3050, RX 6600, Apple Pro) | Native Res @ **100% 1:1 Scale** | 16× | 2× Resolution | **60 FPS** | 100 |
| **Handheld / Integrated APU** | Integrated Graphics (Intel Iris Xe / UHD, AMD Vega APU, Steam Deck) | Native Res @ **game (5/6 Scale)** | game | 1× Resolution | **60 FPS / 40 FPS** | 100 |

> [!NOTE]
> On integrated APUs and low-power hardware, `render_scale` is kept at `game` (the native 5/6 scale) and `fov` at `100` to prevent draw-call and geometry overhead, delivering maximum frame rate stability.

* **Zero Invasive System Changes:** The optimizer strictly edits `totk.ini` and never modifies system registry settings or terminates background OS processes.

---

## 🚀 How to Run the Optimizer

### Windows
Run via terminal or double-click:
```cmd
python optimize_totk.py
```
*(Or run `Optimize_TotK_PC.bat` / `Optimize_TotK_PC.ps1`)*

### Linux & Steam Deck
```bash
python3 optimize_totk.py
```

### macOS
```bash
python3 optimize_totk.py
```

*Zero external dependencies required — runs on standard Python 3.8+ or PowerShell out of the box.*
