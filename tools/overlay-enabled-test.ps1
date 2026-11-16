<#
.SYNOPSIS
  Pinpoint test: a mod that is switched off gets no file overlay (balance.bin etc. stay the game's own).
.DESCRIPTION
  Run in a COPY of the game folder (never the player's): makes mods\ovltest (overlay of bin\balance.bin), starts the game
  twice with -gear-auto and its own gear_mods.json (off, then on), and checks SacredGear.log of each instance.
  Needs the game lock (tools\game-lock.ps1) is taken here. Exit code 0 = pass.
#>
param([Parameter(Mandatory = $true)][string]$GameDir)
$ErrorActionPreference = 'Stop'
$lock = Join-Path $PSScriptRoot 'game-lock.ps1'
$mod = Join-Path $GameDir 'mods\ovltest'
New-Item -ItemType Directory -Force "$mod\data" | Out-Null
"return {id='ovltest', name='ovltest', version='0.0.1', author='test', description='overlay enable test', automation=true}" | Set-Content "$mod\mod.lua"
'-- empty' | Set-Content "$mod\main.lua"
'bin\balance.bin = data\balance.bin' | Set-Content "$mod\overlay.txt"
'x' | Set-Content "$mod\data\balance.bin"
& $lock acquire -Id overlay-enabled-test
$fail = 0
try {
    foreach ($case in @(@('ovl-off', 'false', 'switched off'), @('ovl-on', 'true', '1 file rule'))) {
        $inst = $case[0]
        $dir = Join-Path $GameDir "GearAutomation\$inst"
        New-Item -ItemType Directory -Force $dir | Out-Null
        "{`"mods`":{`"ovltest`":{`"enabled`":$($case[1]),`"settings`":{`"a`":`"{`"}}},`"version`":1}" | Set-Content "$dir\gear_mods.json"
        $p = Start-Process (Join-Path $GameDir 'Sacred.exe') -WorkingDirectory $GameDir -PassThru `
            -ArgumentList '-gear-auto', '-gear-mods=ovltest', "-gear-instance=$inst", '-gear-timeout=25'
        if (-not $p.WaitForExit(60000)) { $p.Kill() }
        $log = Get-Content "$dir\SacredGear.log" -Raw
        if ($log -match [regex]::Escape($case[2]) -and (($case[1] -eq 'false') -eq ($log -notmatch 'file functions are hooked'))) { "PASS $inst" }
        else { "FAIL $inst"; $fail++ }
    }
} finally { & $lock release -Id overlay-enabled-test }
exit $fail
