<#
.SYNOPSIS
    Adds the language of another Sacred Gold install (text and speech) to a game folder.
.DESCRIPTION
    Takes the text from the other install's sacred.exe (the GOG builds carry it inside the exe, as BINARY resource 107;
    builds without it read scripts\<code>\global.res, which is taken instead) and its PAK\sound.pak, and stores them as
    the files SacredBild loads for the game language <code>:
        scripts\<code>\global.res
        PAK\sound.<code>.pak
    Select the language with "LANGUAGE : <code>" in settings.cfg, or start the game with the code in lower case
    (sacred.exe de). Without these files, the game uses its own text and PAK\sound.pak.
.EXAMPLE
    .\import-language.ps1 -From "B:\Spiele\GOG Games\Sacred Gold" -Language DE -GameDir "B:\Spiele\GOG Games\sacred gold GOG"
    German text and speech from the German install, for LANGUAGE : DE in the English one.
#>
param(
    # The install (or its sacred.exe) whose language to take.
    [Parameter(Mandatory)][string]$From,
    # The game language code to store it under.
    [Parameter(Mandatory)][ValidateSet("US", "DE", "FR", "SP", "IT", "PL", "HU", "JP", "VC", "RU", "CZ")][string]$Language,
    # The game folder to add it to.
    [string]$GameDir = "B:\Spiele\GOG Games\Sacred Gold",
    # Only the text, no speech.
    [switch]$NoSpeech,
    # Hard-link PAK\sound.pak instead of copying it (both installs on the same drive).
    [switch]$HardLink
)
$ErrorActionPreference = "Stop"

if (-not ("SacredText" -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class SacredText
{
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
    [DllImport("kernel32")]
    static extern bool FreeLibrary(IntPtr module);
    [DllImport("kernel32", CharSet = CharSet.Unicode)]
    static extern IntPtr FindResourceW(IntPtr module, IntPtr name, string type);
    [DllImport("kernel32")]
    static extern uint SizeofResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32")]
    static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32")]
    static extern IntPtr LockResource(IntPtr data);

    // The exe's text as global.res, or null if it has none. BINARY resource 107 stores each uint16 XORed with the one
    // before it, the first with 0x45AD (sacred.exe's cTextTable_load).
    public static byte[] FromExe(string exe)
    {
        IntPtr module = LoadLibraryExW(exe, IntPtr.Zero, 0x22); // LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE
        if (module == IntPtr.Zero)
            throw new Win32Exception();
        try
        {
            IntPtr resource = FindResourceW(module, (IntPtr)107, "BINARY");
            if (resource == IntPtr.Zero)
                return null;
            int size = (int)SizeofResource(module, resource);
            byte[] raw = new byte[size];
            Marshal.Copy(LockResource(LoadResource(module, resource)), raw, 0, size);
            byte[] text = (byte[])raw.Clone();
            for (int i = size / 2 - 1; i >= 0; i--)
            {
                int previous = i > 0 ? raw[2 * i - 2] | raw[2 * i - 1] << 8 : 0x45AD;
                int word = (raw[2 * i] | raw[2 * i + 1] << 8) ^ previous;
                text[2 * i] = (byte)word;
                text[2 * i + 1] = (byte)(word >> 8);
            }
            return text;
        }
        finally
        {
            FreeLibrary(module);
        }
    }

    // The number of strings if `data` is a global.res the game can read (uint32 count, count entries {id, offset,
    // flags, bytes} with ascending ids, each string at 4 + offset inside the file), else 0.
    public static uint Count(byte[] data)
    {
        if (data == null || data.Length < 4)
            return 0;
        uint count = BitConverter.ToUInt32(data, 0);
        if (count == 0 || count > (data.Length - 4) / 16)
            return 0;
        for (uint i = 0; i < count; i++)
        {
            int entry = 4 + (int)i * 16;
            uint id = BitConverter.ToUInt32(data, entry);
            ulong end = 4ul + BitConverter.ToUInt32(data, entry + 4) + BitConverter.ToUInt32(data, entry + 12);
            if ((i > 0 && id <= BitConverter.ToUInt32(data, entry - 16)) || end > (ulong)data.Length)
                return 0;
        }
        return count;
    }
}
'@
}

# Sounds in a sound pak ("SND", a version byte, then the count), or $null if it isn't one.
function Get-SoundCount([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    $stream = [System.IO.File]::OpenRead($path)
    try {
        $header = New-Object byte[] 8
        if ($stream.Read($header, 0, 8) -ne 8 -or [System.Text.Encoding]::ASCII.GetString($header, 0, 3) -ne "SND") { return $null }
        return [BitConverter]::ToUInt32($header, 4)
    }
    finally { $stream.Dispose() }
}

$code = $Language.ToUpperInvariant()
$folder = $Language.ToLowerInvariant()
if (Test-Path -LiteralPath $From -PathType Leaf) {
    $sourceExe = (Resolve-Path -LiteralPath $From).Path
    $sourceDir = Split-Path $sourceExe
}
else {
    $sourceDir = (Resolve-Path -LiteralPath $From).Path
    $sourceExe = Join-Path $sourceDir "sacred.exe"
    if (-not (Test-Path -LiteralPath $sourceExe)) { throw "sacred.exe not found in $sourceDir" }
}
if (-not (Test-Path -LiteralPath (Join-Path $GameDir "sacred.exe"))) { throw "sacred.exe not found in $GameDir" }

# Text
$text = [SacredText]::FromExe($sourceExe)
$textSource = "$sourceExe (built-in text)"
if ($null -eq $text) {
    $file = Join-Path $sourceDir "scripts\$folder\global.res"
    if (-not (Test-Path -LiteralPath $file)) { throw "$sourceExe has no built-in text, and there is no $file" }
    $text = [System.IO.File]::ReadAllBytes($file)
    $textSource = $file
}
$count = [SacredText]::Count($text)
if ($count -eq 0) { throw "The text from $textSource is not a global.res the game can read" }

# Speech: checked before anything is written
$soundSource = Join-Path $sourceDir "PAK\sound.pak"
$soundTarget = Join-Path $GameDir "PAK\sound.$folder.pak"
if (-not $NoSpeech) {
    $sounds = Get-SoundCount $soundSource
    $ownSounds = Get-SoundCount (Join-Path $GameDir "PAK\sound.pak")
    if ($null -eq $sounds) { throw "$soundSource is missing or not a sound pak" }
    if ($null -ne $ownSounds -and $sounds -ne $ownSounds) {
        throw "$soundSource has $sounds sounds, $GameDir\PAK\sound.pak has ${ownSounds}: SacredBild would not use it"
    }
}

$textTarget = Join-Path $GameDir "scripts\$folder\global.res"
if ((Test-Path -LiteralPath $textTarget) -and
    [System.Collections.StructuralComparisons]::StructuralEqualityComparer.Equals([System.IO.File]::ReadAllBytes($textTarget), $text)) {
    Write-Host "$textTarget is already this text"
}
else {
    $backup = "$textTarget.sacredbild-backup"
    if ((Test-Path -LiteralPath $textTarget) -and -not (Test-Path -LiteralPath $backup)) {
        Copy-Item -LiteralPath $textTarget $backup
        Write-Host "Backed up the previous $textTarget to $backup"
    }
    New-Item -ItemType Directory -Force (Split-Path $textTarget) | Out-Null
    [System.IO.File]::WriteAllBytes($textTarget, $text)
    Write-Host "Wrote $textTarget ($count strings, from $textSource)"
}

if (-not $NoSpeech) {
    $src = Get-Item -LiteralPath $soundSource
    $dst = Get-Item -LiteralPath $soundTarget -ErrorAction SilentlyContinue
    if ($dst -and $dst.Length -eq $src.Length -and $dst.LastWriteTimeUtc -eq $src.LastWriteTimeUtc) {
        Write-Host "$soundTarget is already there"
    }
    elseif ($HardLink) {
        if ($dst) { Remove-Item -LiteralPath $soundTarget }
        New-Item -ItemType HardLink -Path $soundTarget -Target $soundSource | Out-Null
        Write-Host "Linked $soundTarget to $soundSource"
    }
    else {
        Write-Host ("Copying $soundSource to $soundTarget ({0:N0} MB) ..." -f ($src.Length / 1MB))
        Copy-Item -LiteralPath $soundSource $soundTarget -Force
        Write-Host "Wrote $soundTarget"
    }
}

$setting = Get-Content -LiteralPath (Join-Path $GameDir "settings.cfg") -ErrorAction SilentlyContinue |
    Where-Object { $_ -match '^\s*LANGUAGE\s*:' } | Select-Object -First 1
Write-Host "Done. Select it with 'LANGUAGE : $code' in settings.cfg (now: '$("$setting".Trim())'), or start sacred.exe $folder."
Write-Host "SacredBild.log names the files the game uses (lines starting with 'Language:')."
