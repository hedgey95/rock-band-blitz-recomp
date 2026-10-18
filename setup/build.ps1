# Builds Rock Band Blitz from your own copy of the game, on this PC.
#   build.ps1 -Package <game package> -Keys <title_keys.bin> -InstallDir <folder> [-Shortcut]
#   build.ps1 -InstallTools      installs the missing build tools with winget
# Setup.cmd runs this from a window; it also works on its own. Every step is
# skipped when its result is already there, so running it again resumes.
# The log is <InstallDir>\setup.log; -StatusFile gets one line of progress
# ("step|total|message", or "done|..." / "error|...") for the window.
param(
  [string]$Package, [string]$Keys, [string]$InstallDir, [string]$StatusFile,
  [switch]$Shortcut, [switch]$InstallTools
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
$repo = Split-Path $PSScriptRoot -Parent
$steps = 8

function Status([string]$text) {
  if ($StatusFile) { Set-Content -LiteralPath $StatusFile -Value $text -Encoding UTF8 }
}

if ($InstallTools) {
  foreach ($tool in Get-BlitzTools | Where-Object { -not $_.Path }) {
    Write-Host "Installing $($tool.Name)..."
    & winget install -e @($tool.Winget) --accept-package-agreements --accept-source-agreements
  }
  return
}

$InstallDir = [System.IO.Path]::GetFullPath($InstallDir)
New-Item -ItemType Directory -Force $InstallDir | Out-Null
$log = Join-Path $InstallDir 'setup.log'
$step = 0

function Step([string]$text) {
  $script:step++
  Status "$script:step|$steps|$text"
  Add-Content -LiteralPath $log -Value "`r`n=== [$script:step/$steps] $text ($(Get-Date -Format s))"
}

# Runs a tool, its output appended to the log; stops the build if it fails.
function Run([string]$exe, [string[]]$arguments, [string]$cwd = $repo) {
  Add-Content -LiteralPath $log -Value "> $exe $($arguments -join ' ')"
  $quoted = ($arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
  $process = Start-Process -FilePath 'cmd.exe' -WorkingDirectory $cwd -NoNewWindow -Wait -PassThru `
    -ArgumentList "/s /c `"`"$exe`" $quoted >> `"$log`" 2>&1`""
  if ($process.ExitCode -ne 0) { throw "$(Split-Path $exe -Leaf) failed (exit $($process.ExitCode)); see $log" }
}

# Applies a patch unless it already is.
function Apply-Patch([string]$git, [string]$dir, [string]$patch) {
  # (Windows PowerShell stops on a native tool's stderr under 'Stop', even redirected.)
  $ErrorActionPreference = 'Continue'
  & $git -C $dir apply --reverse --check $patch 2>&1 | Out-Null
  $applied = $LASTEXITCODE -eq 0
  $ErrorActionPreference = 'Stop'
  if (-not $applied) { Run $git @('-C', $dir, 'apply', $patch) }
}

try {
  # 1. Inputs and tools
  Step 'Checking the game package, keys and tools'
  $problem = Test-BlitzPackage $Package
  if (-not $problem) { $problem = Test-BlitzKeys $Keys }
  if ($problem) { throw $problem }
  $missing = @(Get-BlitzTools | Where-Object { -not $_.Path } | ForEach-Object { $_.Name })
  if ($missing.Count) { throw "Missing build tools: $($missing -join ', '). Use 'Install missing tools' first." }
  Add-BlitzToolsToPath
  $git = (Get-BlitzTools | Where-Object Name -eq 'Git').Path
  $cmake = (Get-BlitzTools | Where-Object Name -eq 'CMake').Path

  # 2. ReXGlue SDK at its pinned tag, with this project's patch
  Step 'Getting the ReXGlue SDK'
  $sdk = Join-Path $repo 'sdk'
  if (-not (Test-Path "$sdk\.git")) {
    Run $git @('clone', '--branch', 'v0.10.0', '--recursive', 'https://github.com/rexglue/rexglue-sdk.git', $sdk)
  }
  Apply-Patch $git $sdk "$repo\patches\rexglue-sdk.diff"

  # 3. Build and install it (Release has the rexglue tool; the game links RelWithDebInfo)
  Step 'Building the SDK (the longest step)'
  $install = "$sdk\out\install\win-amd64"
  $stamp = "$sdk\out\setup.stamp"
  $patchHash = (Get-FileHash "$repo\patches\rexglue-sdk.diff").Hash
  if (-not ((Test-Path $stamp) -and (Get-Content $stamp) -eq $patchHash -and (Test-Path "$install\bin\rexglue.exe"))) {
    Run $cmake @('--preset', 'win-amd64') $sdk
    Run $cmake @('--build', 'out/build/win-amd64', '--config', 'Release', '--target', 'install') $sdk
    Run $cmake @('--build', 'out/build/win-amd64', '--config', 'RelWithDebInfo', '--target', 'install') $sdk
    Set-Content $stamp $patchHash
  }
  $rexglueDir = "$install\lib\cmake\rexglue" -replace '\\', '/'

  # 4. XenosRecomp (shader translation, built into the game)
  Step 'Getting XenosRecomp'
  $xenos = Join-Path $repo 'thirdparty\XenosRecomp'
  if (Test-Path "$repo\.git") {
    Run $git @('submodule', 'update', '--init', 'thirdparty/XenosRecomp')
  } elseif (-not (Test-Path "$xenos\XenosRecomp")) {  # a zip download has no submodules
    Run $git @('clone', 'https://github.com/hedge-dev/XenosRecomp', $xenos)
    Run $git @('-C', $xenos, 'checkout', '990d03b')
  }
  Run $git @('-C', $xenos, 'submodule', 'update', '--init', '--depth', '1',
             'thirdparty/fmt', 'thirdparty/xxHash', 'thirdparty/zstd', 'thirdparty/smol-v')
  Apply-Patch $git $xenos "$repo\patches\xenos-recomp.diff"

  # 5. Unpack the game package; assets points at it
  Step 'Unpacking your game package'
  $game = Join-Path $InstallDir 'game'
  if (-not (Test-Path "$game\default.xex")) {
    Run $cmake @('-S', 'tools/extract_package', '-B', 'out/build/extract_package', '-G', 'Ninja',
                 '-DCMAKE_CXX_COMPILER=clang++', '-DCMAKE_BUILD_TYPE=RelWithDebInfo', "-Drexglue_DIR=$rexglueDir")
    Run $cmake @('--build', 'out/build/extract_package')
    Run "$repo\out\build\extract_package\extract_package.exe" @($Package, $game)
    if (-not (Test-Path "$game\default.xex")) { throw 'The package unpacked, but default.xex is not in it.' }
  }
  $assets = Join-Path $repo 'assets'
  if (Test-Path $assets) {
    $item = Get-Item $assets -Force
    if (-not ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
      throw "$assets is a real folder; move it away (setup only replaces a link there)."
    }
    cmd /c rmdir "$assets"  # removes the link only, never its target
  }
  cmd /c mklink /J "$assets" "$game" | Out-Null

  # 6. Recompile the game's code (C++ in generated/default, never shared)
  Step 'Recompiling the game code'
  Run "$install\bin\rexglue.exe" @('codegen', 'blitz_manifest.toml')

  # 7. Build blitz.exe
  Step 'Building the game (several minutes)'
  Run $cmake @('--preset', 'win-amd64-relwithdebinfo', "-Drexglue_DIR=$rexglueDir")
  Run $cmake @('--build', 'out/build/win-amd64-relwithdebinfo', '--target', 'blitz')
  $exeDir = Join-Path $repo 'out\build\win-amd64-relwithdebinfo'
  foreach ($dll in 'rexruntimerd.dll', 'rexgpu-xenosrd.dll') {  # copied only when blitz.exe relinks
    Copy-Item "$install\bin\$dll" $exeDir -Force -ErrorAction SilentlyContinue
  }

  # 8. Settings: keys, shader cache and songs folder in the install folder
  Step 'Writing the settings'
  Copy-Item -LiteralPath $Keys (Join-Path $InstallDir 'title_keys.bin') -Force
  foreach ($dir in 'shaders', 'custom_songs') { New-Item -ItemType Directory -Force (Join-Path $InstallDir $dir) | Out-Null }
  $settings = [ordered]@{
    game_data_root = $game; title_keys = Join-Path $InstallDir 'title_keys.bin'; gpu_plugin = 'xenos'
    native_renderer = $true; native_shader_dir = Join-Path $InstallDir 'shaders'
    custom_songs_dir = Join-Path $InstallDir 'custom_songs'
  }
  $toml = Join-Path $exeDir 'blitz.toml'
  $lines = @()
  if (Test-Path $toml) { $lines = @(Get-Content $toml | Where-Object { $_ -notmatch "^($($settings.Keys -join '|')) =" }) }
  else { $lines = @('# Rock Band Blitz settings, written by setup; the hub (F1) changes them.', 'fullscreen = true') }
  foreach ($key in $settings.Keys) {
    $value = $settings[$key]
    if ($value -is [bool]) { $lines += "$key = $($value.ToString().ToLower())" }
    else { $lines += "$key = `"$($value -replace '\\', '\\' -replace '"', '\"')`"" }
  }
  Set-Content -LiteralPath $toml -Value $lines -Encoding ASCII

  if ($Shortcut) {
    $link = (New-Object -ComObject WScript.Shell).CreateShortcut("$([Environment]::GetFolderPath('Desktop'))\Rock Band Blitz.lnk")
    $link.TargetPath = Join-Path $exeDir 'blitz.exe'
    $link.WorkingDirectory = $exeDir
    $link.Save()
  }
  Add-Content -LiteralPath $log -Value "`r`n=== done ($(Get-Date -Format s))"
  Status "done|$steps|$(Join-Path $exeDir 'blitz.exe')"
} catch {
  Add-Content -LiteralPath $log -Value "`r`n=== FAILED: $($_.Exception.Message)"
  Status "error|$step|$($_.Exception.Message)"
  Write-Error $_.Exception.Message
  exit 1
}
