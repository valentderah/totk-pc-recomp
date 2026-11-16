# CPU-only adapter contracts and complete changed-source compilation.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $out = 'build/checks/fsr4-adapter-worker'
    New-Item -ItemType Directory -Force $out | Out-Null
    $clang = 'C:/msys64/ucrt64/bin/clang.exe'
    $headers = @('-I','runtime','-I','external/ReScaleFrame/runtime/orchestrator/include',
        '-I','external/ReScaleFrame/runtime/contract/include','-I','external/ReScaleFrame/sdk/game/include')
    $flags = @('-std=gnu11','-O2','-g','-Wall','-Werror','-fno-strict-aliasing','-march=x86-64-v3',
        '-D_WIN32_WINNT=0x0A00','-DRT_CPU_LEVEL=3','-I','generated/v143','-I','build/v143-rsf',
        '-I','C:/msys64/ucrt64/include/vma','-I','runtime/third_party/imgui') + $headers
    foreach ($name in @('rsf_sr','rsf_split','vk_sync','vk_rec','vk_device','vk_dlaa','vk_exec')) {
        & $clang @flags -c "runtime/$name.c" -o "$out/$name.o"
        if ($LASTEXITCODE) { throw "Compile failed: $name" }
    }
    & $clang -std=gnu11 -O1 -Wall -Werror @headers tests/rsf_sr_lifetime_check.c runtime/rsf_sr.c -o "$out/lifetime.exe"
    if ($LASTEXITCODE) { throw 'Lifetime fixture compile failed' }
    & "$out/lifetime.exe"
    if ($LASTEXITCODE) { throw 'Lifetime fixture failed' }
    & $clang -std=gnu11 -O1 -Wall -Werror @headers tests/rsf_split_submit_check.c runtime/rsf_split.c -o "$out/split-submit.exe"
    if ($LASTEXITCODE) { throw 'Submit fixture compile failed' }
    & "$out/split-submit.exe"
    if ($LASTEXITCODE) { throw 'Submit fixture failed' }
    & python tests/rsf_split_record_check.py
    if ($LASTEXITCODE) { throw 'Actual-function fixtures failed' }
    Write-Output 'FSR4 adapter: seven complete source objects and seven CPU fixtures passed'
} finally {
    Pop-Location
}
