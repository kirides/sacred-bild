<#
.SYNOPSIS
    Removes SacredBild from a Sacred Gold folder and restores the previous ddraw.dll.
#>
param([string]$GameDir = "B:\Spiele\GOG Games\Sacred Gold")
$ErrorActionPreference = "Stop"

$target = Join-Path $GameDir "ddraw.dll"
$backup = Join-Path $GameDir "ddraw.dll.sacredbild-backup"
$chain = Join-Path $GameDir "SacredBild\DDrawCompat.dll"

if ((Test-Path $target) -and ((Get-Item $target).VersionInfo.ProductName -ne "SacredBild")) {
    throw "ddraw.dll in $GameDir is not SacredBild; nothing to uninstall."
}
Remove-Item -Force -ErrorAction SilentlyContinue $target, (Join-Path $GameDir "ddraw.pdb")

if (Test-Path $backup) {
    Move-Item -Force $backup $target
    Write-Host "Restored previous ddraw.dll from backup"
}
elseif (Test-Path $chain) {
    Copy-Item $chain $target
    Write-Host "Restored DDrawCompat from $chain"
}
Write-Host "SacredBild removed. SacredBild.ini, logs and the SacredBild folder were left in place."
