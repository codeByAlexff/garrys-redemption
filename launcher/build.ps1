# Builds the launcher with the C# compiler that ships with .NET Framework 4 (on every
# Windows since 8), so neither building nor running it needs anything installed.
#   powershell -File launcher\build.ps1
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path $csc)) { throw "csc.exe not found at $csc (.NET Framework 4 is part of Windows)." }
$out = Join-Path $here 'build'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'GarrysRedemptionPassthrough.exe'
& $csc /nologo /target:winexe /platform:x64 /optimize+ /out:$exe `
    /r:System.dll /r:System.Core.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll `
    (Join-Path $here 'Launcher.cs')
if ($LASTEXITCODE -ne 0) { throw "The launcher did not compile (exit code $LASTEXITCODE)." }
Write-Host "built:  $exe"
