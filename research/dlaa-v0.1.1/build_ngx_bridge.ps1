param([Parameter(Mandatory=$true)][string]$PackageRoot,[string]$VcVars='')
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$sdkRoot=Join-Path $root 'dependencies\nvidia-dlss'
$signature=Get-AuthenticodeSignature (Join-Path $sdkRoot 'lib\Windows_x86_64\rel\nvngx_dlss.dll')
if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'NVIDIA'){throw 'NVIDIA runtime signature not verified'}
if(-not(Get-Command cl.exe -ErrorAction SilentlyContinue)){
    if(-not $VcVars){
        $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        $installation=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if($LASTEXITCODE -ne 0 -or -not $installation){throw 'Use a Visual Studio developer shell or supply -VcVars'}
        $VcVars=Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
    }
    $environmentLines=& cmd.exe /d /c ('call "'+$VcVars+'" >nul && (set PATH & set INCLUDE & set LIB & set LIBPATH)')
    if($LASTEXITCODE -ne 0){throw 'MSVC environment unavailable'}
    foreach($name in @('PATH','INCLUDE','LIB','LIBPATH')){
        $line=$environmentLines | Where-Object {$_ -like ($name+'=*')} | Select-Object -First 1
        if($line){[Environment]::SetEnvironmentVariable($name,$line.Substring($name.Length+1),'Process')}
    }
}
New-Item -ItemType Directory -Path (Join-Path $root 'build') -Force | Out-Null
& cl.exe /nologo /LD /MT /EHsc /O2 /W4 /std:c++17 ("/I"+$sdkRoot+'\include') ("/I"+$PackageRoot+'\runtime\deps\include') (Join-Path $root 'native\ngx_bridge.cpp') ("/Fo"+$root+'\build\ngx_bridge.obj') ("/Fe"+$root+'\build\ngx_bridge.dll') /link (Join-Path $sdkRoot 'lib\Windows_x86_64\x64\nvsdk_ngx_s.lib') advapi32.lib user32.lib wintrust.lib crypt32.lib shlwapi.lib ("/IMPLIB:"+$root+'\build\ngx_bridge.lib')
if($LASTEXITCODE -ne 0){throw 'NGX bridge compilation failed'}
