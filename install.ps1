<#
.SYNOPSIS
    Installs SacredBild into a Sacred Gold folder.
.DESCRIPTION
    - Backs up the current ddraw.dll once (ddraw.dll.sacredbild-backup).
    - If that ddraw.dll is DDrawCompat, moves it to SacredBild\DDrawCompat.dll so SacredBild can chain-load it.
    - Copies SacredBild's ddraw.dll (+ pdb) and creates SacredBild.ini if it doesn't exist.
#>
param(
    [string]$GameDir = "B:\Spiele\GOG Games\Sacred Gold",
    [string]$BuildDir = (Join-Path $PSScriptRoot "out\build\msvc-x86\RelWithDebInfo")
)
$ErrorActionPreference = "Stop"

$target = Join-Path $GameDir "ddraw.dll"
$chainDir = Join-Path $GameDir "SacredBild"
$chain = Join-Path $chainDir "DDrawCompat.dll"
$backup = Join-Path $GameDir "ddraw.dll.sacredbild-backup"

if (-not (Test-Path (Join-Path $GameDir "sacred.exe"))) { throw "sacred.exe not found in $GameDir" }
if (-not (Test-Path (Join-Path $BuildDir "ddraw.dll"))) { throw "Build output not found in $BuildDir (run: cmake --build --preset release)" }

function Get-Product([string]$path) { (Get-Item $path).VersionInfo.ProductName }

# DDrawCompat builds don't always carry a version resource; look for its name in the binary.
# It is stored as UTF-16 (wide string literals), so search both encodings.
function Test-DDrawCompat([string]$path) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $ascii = [System.Text.Encoding]::ASCII.GetString($bytes)
    $wide = [System.Text.Encoding]::Unicode.GetString($bytes)
    return $ascii.Contains("DDrawCompat") -or $wide.Contains("DDrawCompat")
}

if (Test-Path $target) {
    $product = Get-Product $target
    if ($product -ne "SacredBild") {
        if (-not (Test-Path $backup)) {
            Copy-Item $target $backup
            Write-Host "Backed up existing ddraw.dll to $backup"
        }
        if (Test-DDrawCompat $target) {
            New-Item -ItemType Directory -Force $chainDir | Out-Null
            Move-Item -Force $target $chain
            Write-Host "Moved DDrawCompat to $chain"
        }
        else {
            Write-Warning "Existing ddraw.dll is not DDrawCompat. It stays in the backup only; set [DDraw] Chain in SacredBild.ini to use another wrapper."
        }
    }
}
if (-not (Test-Path $chain)) {
    Write-Warning "No DDrawCompat at $chain: SacredBild will fall back to Windows' ddraw.dll. Get DDrawCompat from https://github.com/narzoul/DDrawCompat/releases and place its ddraw.dll there as DDrawCompat.dll."
}

Copy-Item -Force (Join-Path $BuildDir "ddraw.dll") $target
$pdb = Join-Path $BuildDir "ddraw.pdb"
if (Test-Path $pdb) { Copy-Item -Force $pdb (Join-Path $GameDir "ddraw.pdb") }
Write-Host "Installed SacredBild ddraw.dll"

$ini = Join-Path $GameDir "SacredBild.ini"
if (-not (Test-Path $ini)) {
    Copy-Item (Join-Path $PSScriptRoot "dist\SacredBild.ini") $ini
    Write-Host "Created $ini"
}
Write-Host "Done. Logs: SacredBild.log (and DDrawCompat-sacred.log) in $GameDir"
