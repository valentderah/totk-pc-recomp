@echo off
title Switch Laptop Display to Integrated GPU (Intel UHD)
cls
echo =====================================================================
echo    Laptop GPU Switch Helper - Release dGPU to Integrated GPU (iGPU)
echo =====================================================================
echo.
echo Closing background apps holding onto NVIDIA RTX GPU...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "Stop-Process -Name 'SearchHost','StartMenuExperienceHost','TextInputHost','PhoneExperienceHost','AccountsControlHost','CHXSmartScreen' -Force -ErrorAction SilentlyContinue; Write-Host '[OK] Sticky background shell apps terminated!' -ForegroundColor Green"

echo.
echo Checking NVIDIA MUX switch mode...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$r = 'HKLM:\SYSTEM\CurrentControlSet\Services\nvlddmkm\Global\NvHybrid\Persistence\ACE'; if (Test-Path $r) { $p = Get-ItemProperty $r -ErrorAction SilentlyContinue; if ($p.InternalMuxState -eq 1 -and $p.InternalMuxIsAutomaticMode -eq 0) { Write-Host '[!] NOTICE: Laptop MUX is currently locked to Dedicated GPU Only!' -ForegroundColor Yellow; Write-Host '    To switch back to Intel iGPU, change GPU Mode in ASUS Armoury Crate to Standard, or in NVIDIA Control Panel to Automatic Select.' -ForegroundColor Cyan; } else { Write-Host '[OK] MUX is in Automatic/Optimus mode. Display should now switch to Intel iGPU.' -ForegroundColor Green; } }"

echo.
set /p rst="Do you also want to restart Windows Explorer to release the desktop swapchain? (Y/N): "
if /i "%rst%"=="Y" (
    echo Restarting Windows Explorer...
    taskkill /f /im explorer.exe >nul 2>&1
    start explorer.exe
    echo [OK] Explorer restarted on Intel iGPU.
)

echo.
echo All done! Press any key to close.
pause >nul
