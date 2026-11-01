<#
.SYNOPSIS
    Smart Hardware Optimizer, Laptop GPU Switcher & Fluid Mouse Look Patcher for Zelda: Tears of the Kingdom (PC Recomp)
#>

$host.UI.RawUI.WindowTitle = "TotK PC Smart Hardware Optimizer & GPU Switcher"
Write-Host "=====================================================================" -ForegroundColor Cyan
Write-Host "   Zelda: Tears of the Kingdom - Smart Hardware Optimizer & Fixer    " -ForegroundColor Cyan
Write-Host "=====================================================================" -ForegroundColor Cyan
Write-Host ""

$gameDir = $PSScriptRoot
if (-not (Test-Path (Join-Path $gameDir "totk.exe"))) {
    if (Test-Path "C:\Games\TotK\totk.exe") {
        $gameDir = "C:\Games\TotK"
    } else {
        Write-Host "[ERROR] totk.exe was not found in the current folder or in C:\Games\TotK!" -ForegroundColor Red
        Read-Host "Press Enter to exit"
        exit 1
    }
}

Write-Host "[*] Target Game Directory: $gameDir" -ForegroundColor Gray
Write-Host ""

# Close any running game instances
Get-Process -Name "totk" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800

$exePath = Join-Path $gameDir "totk.exe"
$bakPath = Join-Path $gameDir "totk.exe.bak"
$iniPath = Join-Path $gameDir "totk.ini"

# 1. Hardware Detection
Write-Host "[1/5] Scanning System Hardware..." -ForegroundColor Cyan
$gpus = Get-CimInstance Win32_VideoController
$dedicatedGpu = $gpus | Where-Object { 
    ($_.Name -match 'RTX|GTX|Radeon RX|Arc|GeForce') -and 
    ($_.Name -notmatch 'Integrated|UHD|Iris|Vega\s+[0-9]|Graphics$') 
} | Select-Object -First 1
$primaryGpu = if ($dedicatedGpu) { $dedicatedGpu } else { $gpus | Select-Object -First 1 }

$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$ramBytes = (Get-CimInstance Win32_PhysicalMemory | Measure-Object -Property Capacity -Sum).Sum
$ramGB = [math]::Round(($ramBytes / 1GB), 1)

$screenW = [int]$primaryGpu.CurrentHorizontalResolution
$screenH = [int]$primaryGpu.CurrentVerticalResolution
if ($screenW -le 0 -or $screenH -le 0) {
    Add-Type -AssemblyName System.Windows.Forms
    $screenW = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Width
    $screenH = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds.Height
}

Write-Host ("      CPU     : {0} ({1} Cores)" -f $cpu.Name.Trim(), $cpu.NumberOfCores) -ForegroundColor Gray
Write-Host ("      RAM     : {0} GB" -f $ramGB) -ForegroundColor Gray
Write-Host ("      GPU     : {0}" -f $primaryGpu.Name) -ForegroundColor Gray
Write-Host ("      Display : {0}x{1}" -f $screenW, $screenH) -ForegroundColor Gray
Write-Host ""

# 2. Dynamic Performance Tier Selection
Write-Host "[2/5] Evaluating Best Performance Profile..." -ForegroundColor Cyan
$profile = "Balanced"
$renderScale = 100
$aniso = 16
$shadowScale = 2
$fpsLimit = 60
$upscaler = "bilinear"
$sharpness = 0
$colorPreset = "default"

if ($dedicatedGpu) {
    if ($dedicatedGpu.Name -match '40[6-9]0|30[7-9]0|2080|6800|6900|7700|7800|7900|A770' -or $cpu.NumberOfCores -ge 8) {
        $profile = "High-End / Enthusiast"
        $renderScale = 133
        $aniso = 16
        $shadowScale = 2
        $fpsLimit = 90
        $upscaler = "bilinear"
        $sharpness = 0
        $colorPreset = "clarity"
    } else {
        $profile = "Balanced / Mid-Range"
        $renderScale = 100
        $aniso = 16
        $shadowScale = 2
        $fpsLimit = 60
        $upscaler = "bilinear"
        $sharpness = 0
        $colorPreset = "default"
    }
} else {
    $profile = "Handheld / Low-Power APU"
    $renderScale = 85
    $aniso = 4
    $shadowScale = 1
    $fpsLimit = 40
    $upscaler = "fsr"
    $sharpness = 20
    $colorPreset = "default"
}

Write-Host ("      Selected Tier : {0}" -f $profile) -ForegroundColor Green
Write-Host ("      Configuration : RenderScale={0}%, Aniso={1}x, ShadowScale={2}x, FPS={3}, Target={4}x{5}" -f `
    $renderScale, $aniso, $shadowScale, $fpsLimit, $screenW, $screenH) -ForegroundColor Gray
Write-Host ""

# 3. Fluid PC Mouse-Look Binary Patch
Write-Host "[3/5] Patching totk.exe (Fluid PC Mouse-Look)..." -ForegroundColor Cyan
if (Test-Path $exePath) {
    if (-not (Test-Path $bakPath)) {
        Copy-Item $exePath $bakPath -Force
        Write-Host "      Created safe backup: totk.exe.bak" -ForegroundColor DarkGray
    }
    try {
        $stream = [System.IO.File]::Open($exePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite)
        try {
            # Patch 1: Remove vdivpd throttle in poll_merged
            $div_off = 0xe22241a + 12
            $stream.Seek($div_off, [System.IO.SeekOrigin]::Begin) | Out-Null
            $stream.Write([byte[]]@(0xc5, 0xf9, 0x28, 0xc1), 0, 4)

            # Patch 2: Tune mouse velocity divisor from 3000.0 to 1800.0
            $c_off = 0xeea8d80
            $stream.Seek($c_off, [System.IO.SeekOrigin]::Begin) | Out-Null
            $stream.Write([System.BitConverter]::GetBytes([double]1800.0), 0, 8)

            Write-Host "      [OK] Patched mouse velocity curve & removed 33% rotation throttle!" -ForegroundColor Green
        } finally {
            $stream.Close()
        }
    } catch {
        Write-Host ("      [WARN] Could not patch totk.exe: {0}" -f $_.Exception.Message) -ForegroundColor Yellow
    }
}
Write-Host ""

# 4. Configure Windows DirectX High-Performance GPU Preference & totk.ini
Write-Host "[4/5] Writing Optimal Settings to totk.ini & Windows DirectX Preference..." -ForegroundColor Cyan
try {
    $gpuPrefReg = "HKCU:\Software\Microsoft\DirectX\UserGpuPreferences"
    if (-not (Test-Path $gpuPrefReg)) { New-Item -Path $gpuPrefReg -Force | Out-Null }
    Set-ItemProperty -Path $gpuPrefReg -Name $exePath -Value "GpuPreference=2;"
    Write-Host "      [OK] Windows Graphics Preference set to High Performance (dGPU) for totk.exe" -ForegroundColor Green
} catch {
    Write-Host "      [WARN] Could not set Windows Graphics Preference: $($_.Exception.Message)" -ForegroundColor Yellow
}

if (Test-Path $iniPath) {
    $c = Get-Content $iniPath -Raw
    $c = $c -replace '(?m)^#?\s*output_res\s*=.*', ("output_res = {0}x{1}" -f $screenW, $screenH)
    $c = $c -replace '(?m)^#?\s*window_size\s*=.*', ("window_size = {0}x{1}" -f $screenW, $screenH)
    $c = $c -replace '(?m)^#?\s*fullscreen\s*=.*', "fullscreen = 1"
    $c = $c -replace '(?m)^#?\s*aspect\s*=.*', "aspect = fit"
    $c = $c -replace '(?m)^#?\s*render_scale\s*=.*', ("render_scale = {0}" -f $renderScale)
    $c = $c -replace '(?m)^#?\s*upscaler\s*=.*', ("upscaler = {0}" -f $upscaler)
    $c = $c -replace '(?m)^#?\s*sharpness\s*=.*', ("sharpness = {0}" -f $sharpness)
    $c = $c -replace '(?m)^#?\s*color_preset\s*=.*', ("color_preset = {0}" -f $colorPreset)
    $c = $c -replace '(?m)^#?\s*fps_limit\s*=.*', ("fps_limit = {0}" -f $fpsLimit)
    $c = $c -replace '(?m)^#?\s*realtime\s*=.*', "realtime = 1"
    $c = $c -replace '(?m)^#?\s*cloth_realtime\s*=.*', "cloth_realtime = 1"
    $c = $c -replace '(?m)^#?\s*highfps_fix\s*=.*', "highfps_fix = 1"
    $c = $c -replace '(?m)^#?\s*aniso\s*=.*', ("aniso = {0}" -f $aniso)
    $c = $c -replace '(?m)^#?\s*shadow_scale\s*=.*', ("shadow_scale = {0}" -f $shadowScale)
    $c = $c -replace '(?m)^#?\s*contact_shadows\s*=.*', "contact_shadows = 0"
    $c = $c -replace '(?m)^#?\s*pcss\s*=.*', "pcss = 0"
    $c = $c -replace '(?m)^#?\s*stick_sens\s*=.*', "stick_sens = 3.0"
    $c = $c -replace '(?m)^#?\s*mouse_sens\s*=.*', "mouse_sens = 1.0"
    $c = $c -replace '(?m)^#?\s*fov\s*=.*', "fov = 110"
    $c = $c -replace '(?m)^#?\s*gpu_index\s*=.*', "gpu_index = 0"
    $c = $c -replace '(?m)^#?\s*gpu_name\s*=.*', "gpu_name = NVIDIA"
    [System.IO.File]::WriteAllText($iniPath, $c, [System.Text.Encoding]::UTF8)
    Write-Host "      [OK] totk.ini successfully configured!" -ForegroundColor Green
}
Write-Host ""

# 5. Laptop Display & MUX Switch Diagnostics / Cleanup
Write-Host "[5/5] Diagnosing Laptop GPU & Display Switching..." -ForegroundColor Cyan

$stickyApps = @(
    "SearchHost",
    "StartMenuExperienceHost",
    "TextInputHost",
    "ShellExperienceHost",
    "PhoneExperienceHost",
    "AccountsControlHost",
    "CHXSmartScreen"
)

# Check NVIDIA MUX status in registry if available
$muxReg = "HKLM:\SYSTEM\CurrentControlSet\Services\nvlddmkm\Global\NvHybrid\Persistence\ACE"
$muxState = $null
$muxAuto = $null
if (Test-Path $muxReg) {
    try {
        $muxProps = Get-ItemProperty -Path $muxReg -ErrorAction SilentlyContinue
        $muxState = $muxProps.InternalMuxState
        $muxAuto = $muxProps.InternalMuxIsAutomaticMode
    } catch {}
}

if ($muxState -eq 1 -and $muxAuto -eq 0) {
    Write-Host "      [!] Status: Laptop display is currently locked to 'NVIDIA GPU Only' (or Armoury Crate 'Ultimate' Mode)." -ForegroundColor Yellow
    Write-Host "          In this mode, your screen is hardwired to the RTX GPU and will NOT switch to Intel UHD." -ForegroundColor Yellow
    Write-Host "          -> To enable automatic switching:" -ForegroundColor Cyan
    Write-Host "             1. Open ASUS Armoury Crate -> Set GPU Mode to 'Standard' or 'Optimized'." -ForegroundColor Cyan
    Write-Host "             2. Or open NVIDIA Control Panel -> Manage Display Mode -> Select 'Automatic Select' or 'Optimus'." -ForegroundColor Cyan
} elseif ($muxAuto -eq 1) {
    Write-Host "      [OK] NVIDIA Advanced Optimus is in 'Automatic Select' mode." -ForegroundColor Green
}

# Release sticky background processes that block display switching back to iGPU
Write-Host "      [*] Releasing background shell apps holding onto dedicated GPU..." -ForegroundColor Gray
Stop-Process -Name $stickyApps -Force -ErrorAction SilentlyContinue
Write-Host "      [OK] Background processes cleared (they will auto-restart on the Intel iGPU)." -ForegroundColor Green
Write-Host ""

Write-Host "=====================================================================" -ForegroundColor Green
Write-Host "                    OPTIMIZATION COMPLETE!                           " -ForegroundColor Green
Write-Host "=====================================================================" -ForegroundColor Green
Write-Host ""

$ans = Read-Host "Would you like to launch Zelda: Tears of the Kingdom now? (Y/N)"
if ($ans -match '^[Yy]') {
    Write-Host "Starting totk.exe and monitoring game session..." -ForegroundColor Green
    $gameProc = Start-Process $exePath -WorkingDirectory $gameDir -PassThru
    
    # Wait for game to exit, then cleanly release dGPU back to iGPU
    Write-Host "Game is running. Once you quit the game, dGPU cleanup will run automatically." -ForegroundColor DarkGray
    $gameProc.WaitForExit()
    
    Write-Host ""
    Write-Host "[*] Game closed! Running post-game dGPU display release cleanup..." -ForegroundColor Cyan
    Stop-Process -Name $stickyApps -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
    Write-Host "[OK] Cleaned up! Display can now freely switch to Integrated GPU." -ForegroundColor Green
    Start-Sleep -Seconds 2
}
