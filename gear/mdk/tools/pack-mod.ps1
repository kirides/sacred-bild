<#
.SYNOPSIS  Zips a mod folder so players can extract it into the game's mods folder.
.EXAMPLE   .\tools\pack-mod.ps1 -Mod src\my-mod
#>
param(
    [Parameter(Mandatory)][string]$Mod,
    [string]$Out = '.'
)
$ErrorActionPreference = 'Stop'
$dir = (Resolve-Path $Mod).Path.TrimEnd('\')
if (-not (Test-Path (Join-Path $dir 'mod.lua'))) { throw "$dir has no mod.lua" }
$zip = Join-Path $Out ((Split-Path $dir -Leaf) + '.zip')
if (Test-Path $zip) { Remove-Item $zip }
# The folder itself is the zip's top entry: extracting into mods\ gives mods\<id>\mod.lua.
Compress-Archive -Path $dir -DestinationPath $zip
Write-Host "Wrote $zip"
