# CPU-only metadata lifetime, actual Present wrappers and changing-surface contracts.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Push-Location (Split-Path -Parent $PSScriptRoot)
try {
    $out = 'build/checks/fg-history-worker'
    New-Item -ItemType Directory -Force $out | Out-Null
    $clang = 'C:/msys64/ucrt64/bin/clang.exe'
    $headers = @('-I','runtime','-I','external/ReScaleFrame/runtime/orchestrator/include',
        '-I','external/ReScaleFrame/runtime/contract/include','-I','external/ReScaleFrame/sdk/game/include')
    foreach ($file in @('gfx_present','rsf_fg_history')) {
        & $clang -std=gnu11 -O2 -g -Wall -Werror -fno-strict-aliasing -march=x86-64-v3 `
            -D_WIN32_WINNT=0x0A00 -DRT_CPU_LEVEL=3 @headers -I generated/v143 -I build/v143-rsf `
            -I C:/msys64/ucrt64/include/vma -I runtime/third_party/imgui -c "runtime/$file.c" -o "$out/$file.o"
        if ($LASTEXITCODE) { throw "Compile failed: $file" }
    }
    & $clang -std=gnu11 -O2 -Wall -Werror @headers tests/rsf_fg_history_check.c `
        runtime/rsf_fg_history.c runtime/rsf_fg_map.c -o "$out/history.exe"
    if ($LASTEXITCODE) { throw 'History fixture compile failed' }
    & "$out/history.exe"
    if ($LASTEXITCODE) { throw 'History fixture failed' }
    & python tests/rsf_fg_history_present_check.py
    if ($LASTEXITCODE) { throw 'Actual Present fixture failed' }
    & python tests/rsf_fg_surface_extent_check.py
    if ($LASTEXITCODE) { throw 'Actual surface extent fixture failed' }
    Write-Output 'FG metadata history: two whole-source compiles and three CPU executables passed'
} finally { Pop-Location }
