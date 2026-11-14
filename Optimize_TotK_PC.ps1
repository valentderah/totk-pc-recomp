<#
.SYNOPSIS
    Smart Hardware Optimizer for Zelda: Tears of the Kingdom (PC Recomp)
    Configures totk.ini with optimal graphics, aspect ratio, and performance settings based on system hardware.
#>

$host.UI.RawUI.WindowTitle = "TotK PC Smart Hardware Optimizer"
Write-Host "=====================================================================" -ForegroundColor Cyan
Write-Host "       Zelda: Tears of the Kingdom - Smart Hardware Optimizer        " -ForegroundColor Cyan
Write-Host "=====================================================================" -ForegroundColor Cyan
Write-Host ""

$gameDir = $PSScriptRoot
if (-not (Test-Path (Join-Path $gameDir "totk.ini") -PathType Leaf) -and -not (Test-Path (Join-Path $gameDir "totk.exe") -PathType Leaf)) {
    if (Test-Path "C:\Games\TotK\totk.ini") {
        $gameDir = "C:\Games\TotK"
    } else {
        Write-Host "[ERROR] Could not find totk.ini or totk.exe in current folder or C:\Games\TotK!" -ForegroundColor Red
        Read-Host "Press Enter to exit"
        exit 1
    }
}

Write-Host "[*] Target Game Directory: $gameDir" -ForegroundColor Gray
Write-Host ""

$iniPath = Join-Path $gameDir "totk.ini"
if (-not (Test-Path $iniPath)) {
    $exampleIni = Join-Path $gameDir "totk.ini.example"
    if (Test-Path $exampleIni) {
        Copy-Item $exampleIni $iniPath
        Write-Host "      Created totk.ini from totk.ini.example" -ForegroundColor DarkGray
    }
}

# 1. Hardware Detection
Write-Host "[1/3] Scanning System Hardware..." -ForegroundColor Cyan
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

$isNvidia = $primaryGpu.Name -match 'NVIDIA|GeForce|RTX|GTX'

Write-Host ("      CPU     : {0} ({1} Cores)" -f $cpu.Name.Trim(), $cpu.NumberOfCores) -ForegroundColor Gray
Write-Host ("      RAM     : {0} GB" -f $ramGB) -ForegroundColor Gray
Write-Host ("      GPU     : {0}" -f $primaryGpu.Name) -ForegroundColor Gray
Write-Host ("      Display : {0}x{1}" -f $screenW, $screenH) -ForegroundColor Gray
Write-Host ""

# 2. Performance Profile Selection
Write-Host "[2/3] Evaluating Best Performance Profile..." -ForegroundColor Cyan
$profile = "Balanced"
$renderScale = "100"
$aniso = "16"
$shadowScale = "2"
$fpsLimit = "60"
$upscaler = "bilinear"
$sharpness = "0"
$colorPreset = "default"
$fov = "100"

if ($dedicatedGpu) {
    if ($dedicatedGpu.Name -match '40[6-9]0|30[7-9]0|2080|6800|6900|7700|7800|7900|A770' -or $cpu.NumberOfCores -ge 8) {
        $profile = "High-End / Enthusiast"
        $renderScale = "133"
        $aniso = "16"
        $shadowScale = "2"
        $fpsLimit = "90"
        $upscaler = "bilinear"
        $sharpness = "0"
        $colorPreset = "default"
        $fov = "110"
    } else {
        $profile = "Balanced / Mid-Range"
        $renderScale = "100"
        $aniso = "16"
        $shadowScale = "2"
        $fpsLimit = "60"
        $upscaler = "bilinear"
        $sharpness = "0"
        $colorPreset = "default"
        $fov = "100"
    }
} else {
    # Handheld / Integrated GPU (Intel UHD, Iris Xe, AMD Vega, Steam Deck)
    # Using 'game' render scale (5/6) and default FOV avoids draw-overhead on low-power APUs
    $profile = "Handheld / Integrated APU"
    $renderScale = "game"
    $aniso = "game"
    $shadowScale = "1"
    $fpsLimit = "60"
    $upscaler = "bilinear"
    $sharpness = "0"
    $colorPreset = "default"
    $fov = "100"
}

Write-Host ("      Selected Tier : {0}" -f $profile) -ForegroundColor Green
Write-Host ("      Configuration : RenderScale={0}, Aniso={1}, ShadowScale={2}, FPS={3}, FOV={4}, Resolution={5}x{6}" -f `
    $renderScale, $aniso, $shadowScale, $fpsLimit, $fov, $screenW, $screenH) -ForegroundColor Gray
Write-Host ""

# 3. Apply Settings to totk.ini
Write-Host "[3/3] Writing Optimal Settings to totk.ini..." -ForegroundColor Cyan
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
    $c = $c -replace '(?m)^#?\s*fov\s*=.*', ("fov = {0}" -f $fov)

    # Only set gpu_name if an NVIDIA discrete GPU was specifically identified;
    # otherwise let TotK use its default Vulkan GPU selection (prevents crashing on Intel/AMD).
    if ($isNvidia) {
        $c = $c -replace '(?m)^#?\s*gpu_name\s*=.*', "gpu_name = NVIDIA"
    } else {
        $c = $c -replace '(?m)^gpu_name\s*=.*', "# gpu_name = NVIDIA"
    }

    [System.IO.File]::WriteAllText($iniPath, $c, [System.Text.Encoding]::UTF8)
    Write-Host "      [OK] totk.ini successfully configured!" -ForegroundColor Green
}
Write-Host ""

Write-Host "=====================================================================" -ForegroundColor Green
Write-Host "                    OPTIMIZATION COMPLETE!                           " -ForegroundColor Green
Write-Host "=====================================================================" -ForegroundColor Green
Write-Host ""
