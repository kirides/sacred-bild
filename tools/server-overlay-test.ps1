<#
.SYNOPSIS
  Pinpoint test: the gameserver the game starts opens the mods' overlaid files, and only those of switched-on mods.
.DESCRIPTION
  Run in a COPY of the game folder (never the player's). Installs gear\examples\servertest as mods\servertest (the game's
  own bin\Balance.bin as its overlay file, so the server runs unchanged), then two -gear-auto runs with their own
  gear_mods.json: servertest on (SacredGear-server.log must show the gameserver opening bin\balance.bin from the mod) and
  off (no SacredGear-server.log entry). Takes the game lock. Exit code 0 = pass.
#>
param([Parameter(Mandatory = $true)][string]$GameDir)
$ErrorActionPreference = 'Stop'
$lock = Join-Path $PSScriptRoot 'game-lock.ps1'
$src = Join-Path $PSScriptRoot '..\gear\examples\servertest'
$mod = Join-Path $GameDir 'mods\servertest'
New-Item -ItemType Directory -Force "$mod\data" | Out-Null
Copy-Item "$src\mod.lua", "$src\main.lua", "$src\overlay.txt" $mod -Force
Copy-Item (Join-Path $GameDir 'bin\Balance.bin') "$mod\data\balance.bin" -Force
& $lock acquire -Id server-overlay-test
$fail = 0
try {
    foreach ($case in @(@('srv-on', 'true'), @('srv-off', 'false'))) {
        $inst = $case[0]
        $dir = Join-Path $GameDir "GearAutomation\$inst"
        New-Item -ItemType Directory -Force $dir | Out-Null
        "{`"mods`":{`"servertest`":{`"enabled`":$($case[1])}},`"version`":1}" | Set-Content "$dir\gear_mods.json"
        Remove-Item "$GameDir\SacredGear-server.log" -ErrorAction SilentlyContinue
        $p = Start-Process (Join-Path $GameDir 'Sacred.exe') -WorkingDirectory $GameDir -PassThru `
            -ArgumentList '-gear-auto', '-gear-mods=servertest', "-gear-instance=$inst", '-gear-timeout=90'
        if (-not $p.WaitForExit(120000)) { $p.Kill() }
        $server = if (Test-Path "$GameDir\SacredGear-server.log") { Get-Content "$GameDir\SacredGear-server.log" -Raw } else { '' }
        $opened = $server -match 'the gameserver opened bin\\balance\.bin'
        if (($case[1] -eq 'true') -eq $opened) { "PASS $inst" } else { "FAIL $inst (server log: $server)"; $fail++ }
    }
} finally { & $lock release -Id server-overlay-test }
exit $fail
