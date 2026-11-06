# Build the public Windows installer from source. No game files or console keys are inputs.
# Prerequisites and verification: docs/installer.md and docs/building.md.
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
function Run-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program exited with $LASTEXITCODE" }
}
if (-not (Test-Path 'C:\msys64\ucrt64\bin\ninja.exe')) {
    throw 'Install MSYS2 UCRT64 and its build packages first; see docs/building.md.'
}
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
if (-not (Test-Path '.venv-py314\Scripts\python.exe')) {
    Run-Checked 'uv' @('venv', '--python', '3.14', '.venv-py314')
}
$releasePython = Join-Path $PWD '.venv-py314\Scripts\python.exe'
Run-Checked 'uv' @('pip', 'install', '--python', $releasePython, 'pillow', 'lz4')
if (-not (Test-Path 'refs\MacNativeGames\.git')) {
    Run-Checked 'git' @('clone', '--filter=blob:none', 'https://github.com/ly05010419/MacNativeGames.git', 'refs/MacNativeGames')
}
Run-Checked $releasePython @('tools/build_hactool.py')
Run-Checked $releasePython @('tools/build_xlate.py')
Run-Checked $releasePython @('tools/make_release.py', '--out', 'build/release/TotK-Setup-windows', '--zip', 'build/release/TotK-Setup-independent-buttons-windows.zip')
Get-FileHash 'build/release/TotK-Setup-independent-buttons-windows.zip' -Algorithm SHA256
