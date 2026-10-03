# Collects what tools\build.ps1 built into dist\ and a zip, and installs it into the games
# when their folders are given.
#
#   powershell -File tools\package.ps1
#
#   GR_RDR2_DIR   the folder that holds RDR2.exe          -> GarrysRedemption.asi goes here
#   GR_GMOD_DIR   the folder that holds the garrysmod dir -> module to garrysmod\lua\bin,
#                                                            addon to garrysmod\addons
#
# Without those variables nothing outside the repo is touched. Only this project's own
# binaries and Lua are ever packaged: no game files, no Script Hook.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist 'GarrysRedemption'

$asi = Join-Path $root 'rdr2\build\Release\GarrysRedemption.asi'
$module = Join-Path $root 'gmod\module\build\Release\gmcl_gr_win64.dll'
$moduleServer = Join-Path $root 'gmod\module\build\Release\gmsv_gr_win64.dll'
$addon = Join-Path $root 'gmod\addon'

# The spawn menu's model list is generated, not committed (tools/gen_models.py says why).
python (Join-Path $root 'tools\gen_models.py') --if-missing

function Copy-IfBuilt([string]$file, [string]$toDir, [string]$what) {
    if (-not (Test-Path $file)) {
        Write-Host "skip:   $what is not built"
        return
    }
    try {
        New-Item -ItemType Directory -Force $toDir | Out-Null
        Copy-Item $file $toDir -Force
    } catch {
        if ($_.Exception -isnot [System.UnauthorizedAccessException]) { throw }
        # Works around: a game under Program Files (where the Rockstar launcher installs)
        # cannot be written to without administrator rights. Only this one copy is run
        # elevated, and Windows asks the user first.
        Write-Host "note:   $toDir needs administrator rights: approve the Windows prompt"
        $copy = Start-Process cmd.exe -Verb RunAs -Wait -PassThru -WindowStyle Hidden `
            -ArgumentList "/c `"copy /y `"$file`" `"$toDir`"`""
        if ($copy.ExitCode -ne 0) { throw "Copying $what to $toDir failed (exit code $($copy.ExitCode))." }
    }
    Write-Host "copied: $what -> $toDir"
}

function Copy-Addon([string]$toDir) {
    # Replace rather than merge, so a Lua file deleted from the repo does not linger.
    if (Test-Path $toDir) { Remove-Item $toDir -Recurse -Force -Confirm:$false }
    New-Item -ItemType Directory -Force $toDir | Out-Null
    Copy-Item (Join-Path $addon '*') $toDir -Recurse -Force
    Write-Host "copied: Lua addon -> $toDir"
}

# ---- dist\ : the same layout a user unpacks over their two game folders
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force -Confirm:$false }
Copy-IfBuilt $asi (Join-Path $stage 'RDR2') 'GarrysRedemption.asi'
Copy-IfBuilt $module (Join-Path $stage 'GarrysMod\garrysmod\lua\bin') 'gmcl_gr_win64.dll'
Copy-IfBuilt $moduleServer (Join-Path $stage 'GarrysMod\garrysmod\lua\bin') 'gmsv_gr_win64.dll'
Copy-Addon (Join-Path $stage 'GarrysMod\garrysmod\addons\garrys_redemption')
Copy-Item (Join-Path $root 'tools\play_gmod.bat') (Join-Path $stage 'GarrysMod') -Force
# The launcher sits beside the two folders it installs from.
$launcher = Join-Path $root 'launcher\build\GarrysRedemptionPassthrough.exe'
if (-not (Test-Path $launcher)) { & (Join-Path $root 'launcher\build.ps1') }
Copy-IfBuilt $launcher $stage 'GarrysRedemptionPassthrough.exe'
foreach ($doc in 'README.md', 'LICENSE') {
    $path = Join-Path $root $doc
    if (Test-Path $path) { Copy-Item $path $stage -Force }
}

$zip = Join-Path $dist 'GarrysRedemption.zip'
if (Test-Path $zip) { Remove-Item $zip -Force -Confirm:$false }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Host "zip:    $zip"

# ---- install into the games
if ($env:GR_RDR2_DIR) {
    if (-not (Test-Path (Join-Path $env:GR_RDR2_DIR 'RDR2.exe'))) {
        throw "GR_RDR2_DIR ($env:GR_RDR2_DIR) has no RDR2.exe in it."
    }
    Copy-IfBuilt $asi $env:GR_RDR2_DIR 'GarrysRedemption.asi'
} else {
    Write-Host 'GR_RDR2_DIR is not set: nothing installed into RDR2.'
}

if ($env:GR_GMOD_DIR) {
    $gm = Join-Path $env:GR_GMOD_DIR 'garrysmod'
    if (-not (Test-Path (Join-Path $gm 'lua'))) {
        throw "GR_GMOD_DIR ($env:GR_GMOD_DIR) has no garrysmod\lua folder in it."
    }
    Copy-IfBuilt $module (Join-Path $gm 'lua\bin') 'gmcl_gr_win64.dll'
    Copy-IfBuilt $moduleServer (Join-Path $gm 'lua\bin') 'gmsv_gr_win64.dll'
    Copy-Addon (Join-Path $gm 'addons\garrys_redemption')
} else {
    Write-Host 'GR_GMOD_DIR is not set: nothing installed into GMod.'
}
