<#
.SYNOPSIS
  Machine-wide lock so only one agent at a time launches Sacred.exe / SacredReborn.exe for self tests.

.DESCRIPTION
  acquire  Wait until this -Id owns the lock, then return. Logs the wait every 5 s.
  release  Give the lock back (only if -Id owns it; -Force overrides).
  run      acquire, run the command after the parameters, release (also on error / Ctrl-C), exit with its exit code.
  status   Print the current holder.

  The lock is one file (default %ProgramData%\sacred-bild\game.lock, override with -LockPath or the
  environment variable SACRED_GAME_LOCK) holding JSON with the owner's -Id, PID, host, start time.
  It is created atomically (CreateNew), so exactly one waiter wins.

  A lock is stale, and taken over, when it is older than -MaxAgeMinutes (default 30), or its -OwnerPid
  (or, with "run", the wrapper's own PID) is no longer running. Re-acquiring with the same -Id refreshes it.

  Exit codes: 0 ok, 2 wait timed out, 3 release refused (not the owner), 1 other error.

.EXAMPLE
  powershell -NoProfile -File tools\game-lock.ps1 run -Id agent-uitest-1 -- B:\Spiele\...\Sacred.exe -gear-auto
  powershell -NoProfile -File tools\game-lock.ps1 acquire -Id agent-uitest-1
  powershell -NoProfile -File tools\game-lock.ps1 release -Id agent-uitest-1
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)][ValidateSet('acquire', 'release', 'run', 'status')][string]$Action,
    [string]$Id,
    [int]$TimeoutSeconds = 3600,
    [int]$MaxAgeMinutes = 30,
    [int]$OwnerPid = 0,
    [string]$LockPath,
    [switch]$Force,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Command
)

$ErrorActionPreference = 'Stop'
if (-not $LockPath) { $LockPath = $env:SACRED_GAME_LOCK }
if (-not $LockPath) {
    $base = $env:ProgramData
    if (-not $base) { $base = $env:TEMP }
    $LockPath = Join-Path (Join-Path $base 'sacred-bild') 'game.lock'
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $LockPath) | Out-Null

function Log([string]$m) { Write-Host ("[game-lock {0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m) }

function Read-Lock {
    # Returns $null if no file; an object with .raw/.data/.age (data $null while the file is empty or unreadable).
    if (-not (Test-Path -LiteralPath $LockPath)) { return $null }
    $raw = $null
    try {
        $fs = [IO.File]::Open($LockPath, 'Open', 'Read', 'ReadWrite,Delete')
        try { $raw = (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
    } catch { return $null }
    $age = $null
    try { $age = (Get-Date).ToUniversalTime() - (Get-Item -LiteralPath $LockPath).LastWriteTimeUtc } catch { return $null }
    $data = $null
    try { if ($raw.Trim()) { $data = $raw | ConvertFrom-Json } } catch { }
    [pscustomobject]@{ raw = $raw; data = $data; age = $age }
}

function Test-Stale($lock) {
    if ($lock.age.TotalMinutes -gt $MaxAgeMinutes) { return "older than $MaxAgeMinutes min" }
    if ($null -eq $lock.data) {
        if ($lock.age.TotalSeconds -gt 10) { return 'unreadable/empty' }
        return $null   # still being written
    }
    $p = [int]$lock.data.pid
    if ($p -gt 0 -and -not (Get-Process -Id $p -ErrorAction SilentlyContinue)) { return "owner pid $p is gone" }
    return $null
}

function Describe($lock) {
    if ($lock.data) { "'{0}' (pid {1}, held {2:n0}s)" -f $lock.data.id, $lock.data.pid, $lock.age.TotalSeconds }
    else { 'unknown (being written)' }
}

# Guards check-and-delete of a stale lock, so two waiters cannot delete a freshly taken lock.
function With-Guard([scriptblock]$body) {
    $m = New-Object Threading.Mutex($false, 'Global\sacred-bild-game-lock-guard')
    $got = $false
    try {
        try { $got = $m.WaitOne(10000) } catch [Threading.AbandonedMutexException] { $got = $true }
        if ($got) { & $body }
    } finally { if ($got) { $m.ReleaseMutex() }; $m.Dispose() }
}

function Try-Create([int]$pidToStore) {
    try {
        $fs = [IO.File]::Open($LockPath, 'CreateNew', 'Write', 'Read')
    } catch [IO.IOException] { return $false }
    try {
        $json = [pscustomobject]@{
            id = $Id; pid = $pidToStore; host = $env:COMPUTERNAME
            acquiredUtc = (Get-Date).ToUniversalTime().ToString('o')
        } | ConvertTo-Json -Compress
        $bytes = [Text.Encoding]::UTF8.GetBytes($json)
        $fs.Write($bytes, 0, $bytes.Length)
    } finally { $fs.Dispose() }
    return $true
}

function Acquire([int]$pidToStore) {
    if (-not $Id) { throw '-Id is required' }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $nextLog = 5
    $announced = $false
    while ($true) {
        if (Try-Create $pidToStore) {
            Log ("acquired by '{0}' after waiting {1:n0}s  ({2})" -f $Id, $sw.Elapsed.TotalSeconds, $LockPath)
            return
        }
        $lock = Read-Lock
        if ($lock) {
            if ($lock.data -and $lock.data.id -eq $Id) {   # re-entrant: refresh
                With-Guard { Remove-Item -LiteralPath $LockPath -Force -ErrorAction SilentlyContinue }
                continue
            }
            $why = Test-Stale $lock
            if ($why) {
                With-Guard {
                    $again = Read-Lock
                    if ($again -and $again.raw -eq $lock.raw) {
                        Log ("taking over stale lock held by {0}: {1}" -f (Describe $lock), $why)
                        Remove-Item -LiteralPath $LockPath -Force -ErrorAction SilentlyContinue
                    }
                }
                continue
            }
            if (-not $announced) { Log ("'{0}' waiting for lock held by {1}" -f $Id, (Describe $lock)); $announced = $true }
        }
        if ($sw.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
            Log ("timed out after {0:n0}s waiting for lock held by {1}" -f $sw.Elapsed.TotalSeconds, $(if ($lock) { Describe $lock } else { '?' }))
            exit 2
        }
        Start-Sleep -Milliseconds 1000
        while ($sw.Elapsed.TotalSeconds -ge $nextLog) {
            $l = Read-Lock
            Log ("'{0}' still waiting: {1}s (holder {2})" -f $Id, $nextLog, $(if ($l) { Describe $l } else { 'none now' }))
            $nextLog += 5
        }
    }
}

function Release {
    $lock = Read-Lock
    if (-not $lock) { Log 'no lock held'; return }
    if (-not $Force -and $lock.data -and $lock.data.id -ne $Id) {
        Log ("not releasing: lock is held by {0}, not '{1}' (use -Force to override)" -f (Describe $lock), $Id)
        exit 3
    }
    With-Guard { Remove-Item -LiteralPath $LockPath -Force -ErrorAction SilentlyContinue }
    Log ("released by '{0}'" -f $Id)
}

switch ($Action) {
    'acquire' {
        Acquire $OwnerPid
    }
    'release' {
        if (-not $Id -and -not $Force) { throw '-Id is required (or -Force)' }
        Release
    }
    'status' {
        $l = Read-Lock
        if ($l) { Log ("held by {0}" -f (Describe $l)); Write-Host $l.raw } else { Log 'free' }
    }
    'run' {
        if (-not $Command -or $Command.Count -eq 0) { throw 'run needs a command after the parameters' }
        $cmd = @($Command)
        if ($cmd[0] -eq '--') { $cmd = @($cmd | Select-Object -Skip 1) }
        Acquire $PID
        $code = 1
        try {
            & $cmd[0] @($cmd | Select-Object -Skip 1)
            $code = if ($null -ne $LASTEXITCODE) { $LASTEXITCODE } else { 0 }
        } finally {
            Release
        }
        exit $code
    }
}
