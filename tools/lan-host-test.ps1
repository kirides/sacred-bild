<#
.SYNOPSIS
  Hosts a real LAN game in the menus (gear\examples\lanhosttest) in a COPY of the game folder, with a mod overlaying binalance.bin.
.DESCRIPTION
  -Patch changes one byte of the overlaid balance.bin, so a gameserver that does NOT read the same file makes the game show
  "IP: Kann nicht verbinden! CODE -10021" (the client's balance.bin check). With Sacred Gear's gameserver overlay the game
  loads the world; SacredGear-server.log in the game folder shows the server opening the mod's balance.bin. Screenshots go to
  out\<Instance>-*.png (step 5 shows the result). Takes the game lock. Needs a hero in the copy's Save folder.
#>
param([Parameter(Mandatory = $true)][string]$GameDir, [string]$Instance = 'lan-host', [switch]$Patch)
$inst = $Instance; $mods = 'lanhosttest'; $patch = $Patch
$g = $GameDir
Copy-Item $PSScriptRoot\..\gear\examples\lanhosttest "$g\mods\" -Recurse -Force
New-Item -ItemType Directory -Force "$g\mods\lanhosttest\data" | Out-Null
$b = [IO.File]::ReadAllBytes("$g\bin\Balance.bin")
if ($patch) { $b[1000] = $b[1000] -bxor 1 }
[IO.File]::WriteAllBytes("$g\mods\lanhosttest\data\balance.bin", $b)
Remove-Item "$g\SacredGear-server.log" -ErrorAction SilentlyContinue
$lock = Join-Path $PSScriptRoot 'game-lock.ps1'
& $lock acquire -Id lanhost | Out-Null
try {
    $p = Start-Process "$g\Sacred.exe" -WorkingDirectory $g -PassThru -ArgumentList '-gear-auto', "-gear-mods=$mods", "-gear-instance=$inst", '-gear-timeout=120'
    if (-not $p.WaitForExit(150000)) { $p.Kill() }
    "exit $($p.ExitCode)"
    Start-Sleep 1
    Get-Process GameServer -ErrorAction SilentlyContinue | Stop-Process -Force
} finally { & $lock release -Id lanhost | Out-Null }
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$g\GearAutomation\$inst\*.bmp" | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); $i.Save("$PSScriptRoot\..\out\$inst-$($_.BaseName).png", 'Png'); $i.Dispose() }
