# Builds every C++ project in the repo and runs every test.
#
#   powershell -File tools\build.ps1            build and test
#   powershell -File tools\build.ps1 -NoTests   build only
#
# CMake does not have to be on PATH: the copy that ships with Visual Studio (or the Build
# Tools) is found through vswhere.

param([switch]$NoTests)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Find-VsPath {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    # -products * includes the Build Tools, which the default product filter leaves out.
    & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}

function Find-CMake {
    $onPath = Get-Command cmake -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $vs = Find-VsPath
    if ($vs) {
        $bundled = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path $bundled) { return $bundled }
    }
    throw 'CMake not found. Install it, or install the "C++ CMake tools" component of Visual Studio.'
}

function Invoke-Checked([string]$what, [scriptblock]$command) {
    & $command
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)." }
}

# The .asi imports Script Hook's functions by decorated name. If the names in
# scripthook_imports.def are not all exported by the DLL the game will actually load, the
# plugin would fail to load in-game with no message, so check here instead.
function Test-ScriptHookExports {
    if (-not $env:GR_RDR2_DIR) { return }
    $dll = Join-Path $env:GR_RDR2_DIR 'ScriptHookRDR2.dll'
    if (-not (Test-Path $dll)) {
        Write-Host "note: no ScriptHookRDR2.dll in GR_RDR2_DIR, its exports were not checked."
        return
    }
    $vs = Find-VsPath
    $dumpbin = $null
    if ($vs) {
        $dumpbin = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Recurse -Filter dumpbin.exe |
            Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $dumpbin) {
        Write-Host "note: dumpbin not found, ScriptHookRDR2.dll's exports were not checked."
        return
    }
    $exports = (& $dumpbin /nologo /exports $dll) -join "`n"
    $wanted = Get-Content (Join-Path $root 'rdr2\scripthook_imports.def') |
        ForEach-Object { $_.Trim() } | Where-Object { $_.StartsWith('?') }
    $missing = @($wanted | Where-Object { -not $exports.Contains($_) })
    if ($missing.Count -gt 0) {
        throw "ScriptHookRDR2.dll does not export: $($missing -join ', '). rdr2/src/scripthook.h does not match this Script Hook version."
    }
    Write-Host "ScriptHookRDR2.dll exports all $($wanted.Count) functions the plugin imports."
}

$cmake = Find-CMake
Write-Host "cmake: $cmake"

foreach ($project in 'protocol', 'rdr2', 'gmod\module') {
    $dir = Join-Path $root $project
    if (-not (Test-Path (Join-Path $dir 'CMakeLists.txt'))) {
        Write-Host "skip:  $project (no CMakeLists.txt yet)"
        continue
    }
    if ($project -eq 'gmod\module' -and
        -not (Test-Path (Join-Path $dir 'third_party\gmod-module-base\include\GarrysMod\Lua\Interface.h'))) {
        Write-Host "skip:  $project (Facepunch's gmod-module-base headers are not in gmod\module\third_party; gmod\module\CMakeLists.txt says how to get them)"
        continue
    }
    Write-Host "`n==== $project"
    # `cmake --build --preset` has no -S: presets are looked up in the current directory.
    Push-Location $dir
    try {
        Invoke-Checked "configure $project" { & $cmake --preset default }
        Invoke-Checked "build $project" { & $cmake --build --preset release }
    } finally {
        Pop-Location
    }
}

Test-ScriptHookExports

# The spawn menu's model list is made here rather than committed (tools/gen_models.py says why).
python (Join-Path $root 'tools\gen_models.py') --if-missing

if (-not $NoTests) {
    Write-Host "`n==== tests"
    & (Join-Path $root 'launcher\build.ps1')
    Invoke-Checked 'gr_tests' { & (Join-Path $root 'protocol\build\Release\gr_tests.exe') }
    Push-Location $root
    try {
        Invoke-Checked 'python tests' { python -m unittest discover -s tools/tests }
    } finally {
        Pop-Location
    }
}

Write-Host "`nbuild ok"
