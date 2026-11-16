$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Push-Location (Split-Path -Parent $PSScriptRoot)
try {
    $out='build/checks/rsf-reduced-quality-worker'
    New-Item -ItemType Directory -Force $out | Out-Null
    $clang='C:/msys64/ucrt64/bin/clang.exe'
    $inc=@('-I','runtime','-I','generated/v143','-I','build/v143-rsf','-I','C:/msys64/ucrt64/include/vma',
        '-I','runtime/third_party/imgui','-I','external/ReScaleFrame/runtime/orchestrator/include',
        '-I','external/ReScaleFrame/runtime/contract/include','-I','external/ReScaleFrame/sdk/game/include')
    foreach($name in @('vk_dlaa','resolution','rsf_sr')) {
        & $clang -std=gnu11 -O2 -g -Wall -Werror -fno-strict-aliasing -march=x86-64-v3 `
            -D_WIN32_WINNT=0x0A00 -DRT_CPU_LEVEL=3 @inc -c "runtime/$name.c" -o "$out/$name.o"
        if($LASTEXITCODE){throw "Compile failed: $name"}
    }
    & python tests/rsf_reduced_quality_check.py
    if($LASTEXITCODE){throw 'Quality/FG actual functions failed'}
    & $clang -std=gnu11 -O1 -Wall -Werror -I runtime tests/rsf_resolution_check.c `
        C:/msys64/ucrt64/lib/libzstd.a -o "$out/resolution.exe"
    if($LASTEXITCODE){throw 'Resolution fixture compile failed'}
    & "$out/resolution.exe"
    if($LASTEXITCODE){throw 'Resolution fixture failed'}
    Write-Output 'Reduced SR/FG: three whole-source objects and six CPU executables passed'
} finally {Pop-Location}
