# Dev aid: boot the game replaying an input tape, save its frame at the given seconds,
# then stop it and report whether it crashed.
#   run-scripted.ps1 -Tape tests\input\title.tape -Name run1 -Shots 20,30 [-Seconds 60] [-Config relwithdebinfo] [-Sound] [-Show]
# Muted unless -Sound: --audio_mute writes silence but keeps the audio clock running.
# The window stays hidden (never on screen, never takes focus) unless -Show; screenshots
# come from inside the game (--screenshot_at), so they work either way.
param(
  [Parameter(Mandatory)][string]$Tape,
  [Parameter(Mandatory)][string]$Name,
  [int[]]$Shots = @(),
  [int]$Seconds = 0,
  [string]$Config = "relwithdebinfo",
  [string[]]$Extra = @(),
  [switch]$Sound,
  [switch]$Show
)
$root = Split-Path $PSScriptRoot -Parent
$logs = Join-Path $root "out\logs"
$exe = Join-Path $root "out\build\win-amd64-$Config\blitz.exe"
$data = (Get-Item (Join-Path $root "assets")).Target | Select-Object -First 1
if (-not $Seconds) { $Seconds = (($Shots | Measure-Object -Maximum).Maximum) + 2 }
$gameArgs = @("--game_data_root=$data", "--gpu_plugin=xenos", "--fullscreen=false",
          "--input_replay=$(Resolve-Path $Tape)", "--log_file=$logs\$Name.log",
          "--audio_mute=$(if ($Sound) { 'false' } else { 'true' })",
          "--window_hidden=$(if ($Show) { 'false' } else { 'true' })",
          "--config_file=none", "--hub_shown=true")  # the player's blitz.toml stays out of tests
if ($Shots) { $gameArgs += @("--screenshot_at=$($Shots -join ',')", "--screenshot_prefix=$logs\$Name") }
$gameArgs += $Extra
Remove-Item "$logs\$Name.log", "$logs\$Name-*.png", "$logs\$Name-*.bmp" -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -ArgumentList $gameArgs -PassThru
$start = Get-Date
while (((Get-Date) - $start).TotalSeconds -lt $Seconds -and -not $p.HasExited) { Start-Sleep -Milliseconds 200 }
$crashed = $p.HasExited
if (-not $crashed) { Stop-Process -Id $p.Id -Force; $p.WaitForExit() }
$secs = [int]((Get-Date) - $start).TotalSeconds
if ($crashed) { "CRASHED after ${secs}s, exit 0x{0:X8}" -f $p.ExitCode } else { "alive after ${secs}s (stopped)" }
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$logs\$Name-*.bmp" | ForEach-Object {
  $png = [IO.Path]::ChangeExtension($_.FullName, ".png")
  $image = [System.Drawing.Image]::FromFile($_.FullName)
  $image.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
  $image.Dispose()
  Remove-Item $_.FullName
  $png
}
Select-String -Path "$logs\$Name.log" -Pattern "\[(critical|error)\]|input_replay" | Select-Object -Last 12 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(220, $_.Line.Length)) }
