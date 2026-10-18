# Shared by Setup.ps1 (the window) and build.ps1 (the pipeline): finding the
# build tools and checking the inputs. Windows PowerShell 5.1.

$script:BlitzTitleId = '5841122D'

# Each tool: how to find it, and its winget package.
function Get-BlitzTools {
  $ninja = @(Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Ninja-build.Ninja*\ninja.exe" -ErrorAction SilentlyContinue) +
           @(Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Links\ninja.exe" -ErrorAction SilentlyContinue)
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  $msvc = $null
  if (Test-Path $vswhere) {
    foreach ($vs in @(& $vswhere -all -products * -property installationPath)) {
      $include = Get-ChildItem "$vs\VC\Tools\MSVC\*\include" -ErrorAction SilentlyContinue | Select-Object -First 1
      if ($include) { $msvc = $include.FullName; break }
    }
  }
  $dxc = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*\x64\dxc.exe" -ErrorAction SilentlyContinue |
         Sort-Object FullName | Select-Object -Last 1
  $clang = "$env:ProgramFiles\LLVM\bin\clang++.exe"
  $clangVersion = $null
  if (Test-Path $clang) { $clangVersion = ((& $clang --version) | Select-Object -First 1) -replace '.*version ([0-9.]+).*', '$1' }
  @(
    [pscustomobject]@{ Name = 'LLVM 20'; Path = $(if ($clangVersion -like '20.*') { $clang });
                       Winget = @('--id', 'LLVM.LLVM', '--version', '20.1.8') }
    [pscustomobject]@{ Name = 'CMake'; Path = $(if (Test-Path "$env:ProgramFiles\CMake\bin\cmake.exe") { "$env:ProgramFiles\CMake\bin\cmake.exe" });
                       Winget = @('--id', 'Kitware.CMake') }
    [pscustomobject]@{ Name = 'Ninja'; Path = $(if ($ninja) { $ninja[0].FullName }); Winget = @('--id', 'Ninja-build.Ninja') }
    [pscustomobject]@{ Name = 'Git'; Path = $(if (Test-Path "$env:ProgramFiles\Git\cmd\git.exe") { "$env:ProgramFiles\Git\cmd\git.exe" });
                       Winget = @('--id', 'Git.Git') }
    # MSVC's C++ headers and the Windows SDK (dxc) come with the Build Tools' C++ workload.
    [pscustomobject]@{ Name = 'VS Build Tools (C++)'; Path = $(if ($msvc -and $dxc) { $msvc });
                       Winget = @('--id', 'Microsoft.VisualStudio.2022.BuildTools', '--override',
                                  '--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended') }
  )
}

# Puts the tools' folders first on PATH (a fresh winget install is not on it yet).
function Add-BlitzToolsToPath {
  foreach ($tool in Get-BlitzTools) {
    if ($tool.Path -and $tool.Name -ne 'VS Build Tools (C++)') { $env:PATH = (Split-Path $tool.Path) + ';' + $env:PATH }
  }
}

# $null if the file is the Rock Band Blitz package, else what is wrong with it.
function Test-BlitzPackage([string]$path) {
  if (-not $path -or -not (Test-Path -LiteralPath $path -PathType Leaf)) { return 'Choose the game package file.' }
  $stream = [System.IO.File]::OpenRead($path)
  try {
    $header = New-Object byte[] 0x364
    if ($stream.Read($header, 0, $header.Length) -ne $header.Length) { return 'This file is too small to be a game package.' }
  } finally { $stream.Close() }
  $magic = [System.Text.Encoding]::ASCII.GetString($header, 0, 4)
  if ($magic -notin @('LIVE', 'PIRS', 'CON ')) { return 'This is not an Xbox 360 package.' }
  $title = ($header[0x360..0x363] | ForEach-Object { $_.ToString('X2') }) -join ''
  if ($title -ne $script:BlitzTitleId) { return "This package is for another game (title $title, not $script:BlitzTitleId)." }
  $type = ($header[0x344..0x347] | ForEach-Object { $_.ToString('X2') }) -join ''
  if ($type -ne '000D0000') { return 'This is Rock Band Blitz content, but not the game itself (look for the file in 000D0000).' }
  return $null
}

# $null if the file looks like the song key file, else what is wrong with it.
function Test-BlitzKeys([string]$path) {
  if (-not $path -or -not (Test-Path -LiteralPath $path -PathType Leaf)) { return 'Choose the song keys file.' }
  if ((Get-Item -LiteralPath $path).Length -ne 64) { return 'The song keys file must be exactly 64 bytes.' }
  return $null
}
