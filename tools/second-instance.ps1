<#
.SYNOPSIS
  Starts a second Sacred (SacredBild + Sacred Gear) next to a running one, for LAN / online tests by hand.

.DESCRIPTION
  Runs <Game>\sacred.exe -gear-instance=<Name> -gear-window=<Window>. The name lifts the single-instance check
  ("Sacred is already running") for this process only, titles the window "[gear:<Name>]" and puts the log in
  <Game>\GearAutomation\<Name>\. Start the first game normally; this is the second.

.EXAMPLE
  powershell -NoProfile -File tools\second-instance.ps1 -Game "B:\Spiele\GOG Games\Sacred Gold"
#>
param(
    [Parameter(Mandatory = $true)][string]$Game,
    [string]$Name = 'two',
    [string]$Window = '1600x900'
)
$exe = Join-Path $Game 'sacred.exe'
if (-not (Test-Path $exe)) { Write-Error "no sacred.exe in $Game"; exit 1 }
Start-Process -FilePath $exe -WorkingDirectory $Game -ArgumentList "-gear-instance=$Name", "-gear-window=$Window"
