param([ValidateSet('Saved','K','L')][string]$DlaaPreset='Saved',[switch]$CpuProfile,[switch]$Automated,[switch]$AutoLoad,[ValidateRange(1,120)][int]$Seconds=22,[Parameter(Mandatory=$true)][string]$TestProfile,[string]$SessionRoot=(Join-Path $PSScriptRoot 'build\sessions'))
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
$exe=Join-Path $root 'build\game\totk.exe'
if(Get-CimInstance Win32_Process -Filter "Name = 'totk.exe'" | Where-Object {$_.ExecutablePath -eq $exe}){throw 'The experimental game is already open.'}
$profile=(Resolve-Path -LiteralPath $TestProfile).Path
# Supply an isolated disposable profile; never overwrite or copy an existing profile here.
$session=Join-Path $SessionRoot (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
New-Item -ItemType Directory -Path $session | Out-Null
$runtime=Join-Path $root 'dependencies\nvidia-dlss\lib\Windows_x86_64\rel'
$signature=Get-AuthenticodeSignature (Join-Path $runtime 'nvngx_dlss.dll')
if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'NVIDIA'){throw 'NVIDIA runtime signature not verified.'}
# Keep diagnostic hooks armed but defer their unused one-frame standard dump beyond practical play sessions.
$settings=@{
    TOTK_DLAA_CAMERA_VALIDATION_CACHE='1';TOTK_DLAA_ACQUIRE_HEALTH_LOADS='1';TOTK_DLAA_SHADER_ID_CACHE='1';TOTK_DLAA_GPU_STATS='0';TOTK_DLAA_PRESET=$null;TOTK_DLAA_INITIAL_PRESET=$null;TOTK_NGX_BUFFERED_REPORTS='1';TOTK_VK_PRESENT='mailbox';TOTK_DLAA_DIRECTORY_CACHE='1';TOTK_MOTION_CPU_STAGE='1';TOTK_DLAA_CPU_STATS='0';TOTK_PROBE_CPU_SHADOW='1';TOTK_DLAA_LIVE='1';TOTK_DLAA_PREVIEW='1';TOTK_MOTION_PROBE='1';TOTK_MOTION_GPU='1';
    TOTK_NGX_PROBE='1';TOTK_NGX_EVALUATE='1';TOTK_NGX_SEQUENCE='1';TOTK_NGX_JITTER_SEQUENCE='1';TOTK_NGX_JITTER_FRAMES='8';
    TOTK_PROJECTION_JITTER_PROBE='1';TOTK_PROJECTION_MOTION_PROBE='1';TOTK_PROJECTION_RECONSTRUCTION_PROBE='1';
    TOTK_DUMP='2147483646';TOTK_DUMP_LOG='2147483645';TOTK_DUMP_DIR=$session;TOTK_DUMP_BINARY_ONLY='1';TOTK_DUMP_SKIP_RUNS='1';
    TOTK_BIND_SKIP='0';TOTK_PUSH_SKIP='0';TOTK_DRS_FEEDBACK_PROBE='1';TOTK_VK_VALIDATION='0';
    TOTK_NGX_BRIDGE=(Join-Path $root 'build\ngx_bridge.dll');TOTK_NGX_RUNTIME_DIR=$runtime;
    TOTK_INPUT_SCRIPT='';TOTK_INPUT='auto';TOTK_REALTIME='1';TOTK_OUTPUT_RES='2560x1440';TOTK_RENDER_SCALE='100';TOTK_DLAA_REPLAY_CAPTURE='0';
    TOTK_GPU_TEST_INVALID_POSE='0';TOTK_GPU_TEST_INVALID_CAMERA='0';TOTK_GPU_TEST_COLOR_DISCONTINUITY='0';TOTK_GPU_TEST_PREVIOUS_POSE='0';
    TOTK_MOTION_OBJECTS='0';TOTK_MOTION_GEOMETRY='0';TOTK_CAMERA_AUDIT='0';TOTK_CAMERA_SLOTS_AUDIT='0';TOTK_GPU_JITTER_PROBE='0'
}
if($DlaaPreset -ne 'Saved'){$settings.TOTK_DLAA_INITIAL_PRESET=$DlaaPreset.ToUpperInvariant()}
if($CpuProfile){$settings.TOTK_DLAA_CPU_STATS='1'}
if($Automated -or $AutoLoad){
    $scriptInput=Join-Path $session 'input.txt'
    $stick=if($Automated){'@0 RSTICK 18000 0 50000'}else{'@0 RSTICK 0 0 1'}
    [IO.File]::WriteAllText($scriptInput,("300 A 4`n520 A 4`n700 A 4`n900 A 4`n"+$stick+"`n"),[Text.UTF8Encoding]::new($false))
    $settings.TOTK_INPUT_SCRIPT=$scriptInput
    if($Automated){$settings.TOTK_INPUT='none'}
}
$prior=@{}
foreach($key in $settings.Keys){
    $prior[$key]=[Environment]::GetEnvironmentVariable($key,'Process')
    if($null -eq $settings[$key]){Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}
    else{[Environment]::SetEnvironmentVariable($key,$settings[$key],'Process')}
}
try{
    $arguments=@('--profile',$profile)
    if($Automated){$arguments+=@('--seconds',[string]$Seconds)}
    & $exe @arguments *> (Join-Path $session 'run.log')
    $code=$LASTEXITCODE
    [IO.File]::WriteAllText((Join-Path $session 'native-exit.json'),("{`"exit_code`":$code}"),[Text.UTF8Encoding]::new($false))
    if($code -ne 0){throw "The experimental DLAA game exited with code $code. Log: $session"}
} finally {foreach($key in $prior.Keys){
    if($null -eq $prior[$key]){Remove-Item -LiteralPath ('Env:'+$key) -ErrorAction SilentlyContinue}
    else{[Environment]::SetEnvironmentVariable($key,$prior[$key],'Process')}
}}
Write-Output "DLAA test ended. Log: $session"
