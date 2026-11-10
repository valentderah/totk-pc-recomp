# Publishes the self-contained launcher to launcher\dist\TotKLauncher (needs the .NET 8 SDK)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $here 'dist\TotKLauncher'
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
dotnet publish (Join-Path $here 'TotKLauncher\TotKLauncher.csproj') -c Release -o $out
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "Done: $out\TotKLauncher.exe"
