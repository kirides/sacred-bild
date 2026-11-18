<#
.SYNOPSIS  Creates a new mod folder from the MDK template.
.EXAMPLE   .\tools\new-mod.ps1 -Id my-mod -Name "My mod"
           .\tools\new-mod.ps1 -Id my-mod -Name "My mod" -Dest "C:\Games\Sacred\mods"
#>
param(
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Id,
    [string]$Name = $Id,
    # Where the mod folder is created; by default the src folder of this MDK (copy the result into the game's mods folder).
    [string]$Dest = (Join-Path (Split-Path $PSScriptRoot -Parent) 'src')
)
$ErrorActionPreference = 'Stop'
$template = Join-Path (Split-Path $PSScriptRoot -Parent) 'template'
$target = Join-Path $Dest $Id
if (Test-Path $target) { throw "$target already exists" }
New-Item -ItemType Directory -Force $target | Out-Null
foreach ($f in 'mod.lua', 'main.lua') {
    $text = [IO.File]::ReadAllText((Join-Path $template $f))
    $text = $text.Replace('@ID@', $Id).Replace('@NAME@', $Name)
    [IO.File]::WriteAllText((Join-Path $target $f), $text, (New-Object Text.UTF8Encoding $false))
}
Write-Host "Created $target"
Write-Host "Copy it to <game folder>\mods\$Id, start the game and switch it on in the Mods screen (F11)."
