# N5 gate: the native renderer against the stock renderer on the One Week mash tape.
#   gate-n5.ps1 [-Name gate] [-Seconds 170]
# Both runs use the frame clocks (SDK patches 9 and 10), so a tape replays frame
# for frame; menus are compared at fixed frames, gameplay counted from the first
# 3D frame (async loading moves the song start a frame or two). The reference
# renders what the game asks for (--anisotropic_override=-1; the SDK forces 4x
# anisotropy by default). Pass: every frame within 1% bad pixels
# (tools/compare_frames.py). Hidden and muted, as every test run.
param([string]$Name = "gate", [int]$Seconds = 170)
$root = Split-Path $PSScriptRoot -Parent
$logs = Join-Path $root "out\logs"
$data = Split-Path ((Get-Item (Join-Path $root "assets")).Target | Select-Object -First 1) -Parent
$common = @("--title_keys=$data\title_keys.bin", "--audio_frame_clock=true", "--clock_frame_lock=true",
            "--screenshot_frames=1300,1500,1750,2050,2400,2550,2800",
            "--screenshot_busy_frames=300,1200,2400,4800")
Remove-Item "$logs\$Name-ref-f*", "$logs\$Name-nat-f*" -ErrorAction SilentlyContinue
$tape = Join-Path $root "tests\input\one_week_mash.tape"
# Capture the output first: cutting the pipeline short would stop the script
# before it converts the screenshots.
$out = & "$PSScriptRoot\run-scripted.ps1" -Tape $tape -Name "$Name-ref" -Seconds $Seconds `
  -Extra ($common + @("--anisotropic_override=-1", "--screenshot_prefix=$logs\$Name-ref"))
"stock: " + $out[0]
$out = & "$PSScriptRoot\run-scripted.ps1" -Tape $tape -Name "$Name-nat" -Seconds $Seconds `
  -Extra ($common + @("--native_renderer=true", "--native_shader_dir=$data\shader-hlsl",
                      "--screenshot_prefix=$logs\$Name-nat"))
"native: " + $out[0]
$start = { param($log) (Select-String -Path $log -Pattern "first busy frame (\d+)").Matches[0].Groups[1].Value }
$ref = & $start "$logs\$Name-ref.log"
$nat = & $start "$logs\$Name-nat.log"
"--- menus"
python "$root\tools\compare_frames.py" "$logs\$Name-ref" "$logs\$Name-nat" | Select-Object -First 8
"--- gameplay (first 3D frame: stock $ref, native $nat)"
python "$root\tools\compare_frames.py" "$logs\$Name-ref" "$logs\$Name-nat" --ref-start $ref --native-start $nat |
  Where-Object { $_ -notmatch "no reference frame" }
