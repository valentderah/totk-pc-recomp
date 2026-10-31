param([Parameter(Mandatory=$true)][string]$PackageRoot,[string]$Python='python')
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$PackageRoot=(Resolve-Path -LiteralPath $PackageRoot).Path
$manifest=Get-Content (Join-Path $root 'package-abi.json') -Raw | ConvertFrom-Json
foreach($entry in $manifest.sha256.PSObject.Properties){
    $actual=(Get-FileHash -LiteralPath (Join-Path $PackageRoot $entry.Name) -Algorithm SHA256).Hash
    if($actual -ne $entry.Value){throw ('Expected v0.1.1 packaged ABI: '+$entry.Name)}
}
New-Item -ItemType Directory -Path (Join-Path $root 'build') -Force | Out-Null
& $Python (Join-Path $root 'compile_motion_shaders.py')
if($LASTEXITCODE -ne 0){throw 'Shader compilation failed'}
$archive=Join-Path $root 'build\libtotkrt.a'
Copy-Item -LiteralPath (Join-Path $PackageRoot 'runtime\libtotkrt.a') -Destination $archive
$includes=@('-I',(Join-Path $root 'native'),'-I',(Join-Path $root 'build'),'-I',(Join-Path $PackageRoot 'runtime'),'-I',(Join-Path $PackageRoot 'runtime\deps\include'),'-I',(Join-Path $PackageRoot 'runtime\deps\include\vma'),'-I',(Join-Path $PackageRoot 'runtime\third_party\imgui'))
foreach($module in @('cfg','ui_text','dlaa_settings','dlaa_gpu_profile','boot','gfx_window','vk_sync','vk_usnap','vk_dump','vk_exec_draw','vk_motion_gpu','vk_camera_gpu','vk_ngx_eval','vk_jitter_probe','vk_projection_probe','vk_pipeline','ngx_probe','vk_device')){
    $object=Join-Path $root ('build\'+$module+'.o')
    & (Join-Path $PackageRoot 'llvm\bin\clang.exe') -O2 -g -Wall -fno-strict-aliasing -march=x86-64-v3 @includes -c (Join-Path $root ('native\'+$module+'.c')) -o $object
    if($LASTEXITCODE -ne 0){throw ('Compilation failed: '+$module)}
    & (Join-Path $PackageRoot 'llvm\bin\llvm-ar.exe') r $archive $object
    if($LASTEXITCODE -ne 0){throw 'Archive update failed'}
}
& (Join-Path $PackageRoot 'llvm\bin\clang++.exe') -std=c++17 -O2 -g -Wall -fno-strict-aliasing -march=x86-64-v3 @includes -c (Join-Path $root 'native\menu.cpp') -o (Join-Path $root 'build\menu.o')
if($LASTEXITCODE -ne 0){throw 'Menu compilation failed'}
& (Join-Path $PackageRoot 'llvm\bin\llvm-ar.exe') r $archive (Join-Path $root 'build\menu.o')
if($LASTEXITCODE -ne 0){throw 'Menu archive update failed'}
Write-Output ('Experimental v0.1.1 runtime archive: '+$archive)
