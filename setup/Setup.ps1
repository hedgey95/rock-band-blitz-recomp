# The setup window (Setup.cmd opens it): pick your game package and song keys,
# install any missing build tools, and build the game. The work is done by
# build.ps1 in the background; this window shows its progress.
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()
. "$PSScriptRoot\common.ps1"

$form = New-Object System.Windows.Forms.Form
$form.Text = 'Rock Band Blitz setup'
$form.Size = New-Object System.Drawing.Size(640, 520)
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.Font = New-Object System.Drawing.Font('Segoe UI', 9)

$y = 12
function Add-Label([string]$text, [int]$top, [int]$height = 20, [bool]$bold = $false) {
  $label = New-Object System.Windows.Forms.Label
  $label.Text = $text
  $label.Location = New-Object System.Drawing.Point(16, $top)
  $label.Size = New-Object System.Drawing.Size(600, $height)
  if ($bold) { $label.Font = New-Object System.Drawing.Font('Segoe UI', 9, [System.Drawing.FontStyle]::Bold) }
  $form.Controls.Add($label)
  $label
}
function Add-PathRow([string]$title, [int]$top, [scriptblock]$browse) {
  Add-Label $title $top 20 $true | Out-Null
  $box = New-Object System.Windows.Forms.TextBox
  $box.Location = New-Object System.Drawing.Point(16, ($top + 22))
  $box.Size = New-Object System.Drawing.Size(500, 24)
  $form.Controls.Add($box)
  $button = New-Object System.Windows.Forms.Button
  $button.Text = 'Browse...'
  $button.Location = New-Object System.Drawing.Point(524, ($top + 21))
  $button.Size = New-Object System.Drawing.Size(84, 26)
  $button.Add_Click($browse)  # runs in script scope, where the row variables exist by then
  $form.Controls.Add($button)
  $note = Add-Label '' ($top + 50) 18
  @{ Box = $box; Note = $note }
}

Add-Label 'Builds the game on this PC from your own copy. Nothing from the game is downloaded or shared.' $y 20 | Out-Null

$package = Add-PathRow '1. Your Rock Band Blitz package (the file inside 5841122D\000D0000)' 40 {
  $dialog = New-Object System.Windows.Forms.OpenFileDialog
  $dialog.Title = 'Rock Band Blitz package'
  if ($dialog.ShowDialog() -eq 'OK') { $package.Box.Text = $dialog.FileName }
}
$keys = Add-PathRow '2. Song keys (a 64-byte title_keys.bin; the songs are silent without it)' 116 {
  $dialog = New-Object System.Windows.Forms.OpenFileDialog
  $dialog.Title = 'Song keys'
  if ($dialog.ShowDialog() -eq 'OK') { $keys.Box.Text = $dialog.FileName }
}
$target = Add-PathRow '3. Install folder (game files, settings, shader cache and custom songs go here)' 192 {
  $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
  if ($dialog.ShowDialog() -eq 'OK') { $target.Box.Text = $dialog.SelectedPath }
}
$target.Box.Text = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Rock Band Blitz'

Add-Label '4. Build tools' 268 20 $true | Out-Null
$tools = Add-Label '' 290 20
$installTools = New-Object System.Windows.Forms.Button
$installTools.Text = 'Install missing tools'
$installTools.Location = New-Object System.Drawing.Point(16, 314)
$installTools.Size = New-Object System.Drawing.Size(160, 28)
$form.Controls.Add($installTools)
$toolsNote = Add-Label 'Uses winget; Windows asks before each install. The Visual Studio Build Tools are a few GB.' 346 18

$shortcut = New-Object System.Windows.Forms.CheckBox
$shortcut.Text = 'Put a shortcut on the desktop'
$shortcut.Checked = $true
$shortcut.Location = New-Object System.Drawing.Point(16, 372)
$shortcut.Size = New-Object System.Drawing.Size(300, 22)
$form.Controls.Add($shortcut)

$build = New-Object System.Windows.Forms.Button
$build.Text = 'Build the game'
$build.Location = New-Object System.Drawing.Point(468, 368)
$build.Size = New-Object System.Drawing.Size(140, 32)
$form.Controls.Add($build)

$progress = New-Object System.Windows.Forms.ProgressBar
$progress.Location = New-Object System.Drawing.Point(16, 410)
$progress.Size = New-Object System.Drawing.Size(592, 18)
$form.Controls.Add($progress)
$status = Add-Label 'The first build takes 20 to 40 minutes; later ones resume where they stopped.' 434 40

function Update-Checks {
  $problem = Test-BlitzPackage $package.Box.Text
  $package.Note.Text = $(if ($problem) { $problem } else { "OK: Rock Band Blitz ($script:BlitzTitleId)" })
  $package.Note.ForeColor = $(if ($problem) { 'Firebrick' } else { 'ForestGreen' })
  $problem = Test-BlitzKeys $keys.Box.Text
  $keys.Note.Text = $(if ($problem) { $problem } else { 'OK' })
  $keys.Note.ForeColor = $(if ($problem) { 'Firebrick' } else { 'ForestGreen' })
  $all = Get-BlitzTools
  $tools.Text = ($all | ForEach-Object { "$($_.Name) $(if ($_.Path) { [char]0x2713 } else { [char]0x2717 })" }) -join '    '
  $missing = @($all | Where-Object { -not $_.Path })
  $installTools.Enabled = $missing.Count -gt 0
  $build.Enabled = -not $script:running -and -not $missing.Count -and
                   -not (Test-BlitzPackage $package.Box.Text) -and -not (Test-BlitzKeys $keys.Box.Text) -and $target.Box.Text
}
$package.Box.Add_TextChanged({ Update-Checks })
$keys.Box.Add_TextChanged({ Update-Checks })
$target.Box.Add_TextChanged({ Update-Checks })

$installTools.Add_Click({
  $status.Text = 'Installing tools in the console window; this window updates when it closes.'
  $installTools.Enabled = $false
  $p = Start-Process powershell -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
         "`"$PSScriptRoot\build.ps1`"", '-InstallTools') -PassThru
  $p.WaitForExit()
  Update-Checks
  $status.Text = $(if ($installTools.Enabled) { 'Some tools are still missing; see the console output.' } else { 'Tools ready.' })
})

$script:running = $false
$statusFile = Join-Path $env:TEMP 'blitz-setup-status.txt'
$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 1000
$timer.Add_Tick({
  if (-not (Test-Path $statusFile)) { return }
  $parts = (Get-Content $statusFile -Raw).Trim() -split '\|', 3
  if ($parts.Count -lt 3) { return }
  if ($parts[0] -eq 'done') {
    $timer.Stop(); $script:running = $false; $progress.Value = $progress.Maximum
    $status.Text = "Done. Start the game from the desktop shortcut or $($parts[2]). Press F1 in the game for settings."
    $status.ForeColor = 'ForestGreen'
    Update-Checks
  } elseif ($parts[0] -eq 'error') {
    $timer.Stop(); $script:running = $false
    $status.Text = "Stopped: $($parts[2]) Run the build again after fixing it; it resumes."
    $status.ForeColor = 'Firebrick'
    Update-Checks
  } else {
    $progress.Maximum = [int]$parts[1]; $progress.Value = [Math]::Max(0, [int]$parts[0] - 1)
    $status.Text = "Step $($parts[0]) of $($parts[1]): $($parts[2])..."
  }
})

$build.Add_Click({
  Remove-Item $statusFile -ErrorAction SilentlyContinue
  $script:running = $true
  $build.Enabled = $false
  $status.ForeColor = 'Black'
  $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-WindowStyle', 'Hidden', '-File', "`"$PSScriptRoot\build.ps1`"",
                 '-Package', "`"$($package.Box.Text)`"", '-Keys', "`"$($keys.Box.Text)`"",
                 '-InstallDir', "`"$($target.Box.Text)`"", '-StatusFile', "`"$statusFile`"")
  if ($shortcut.Checked) { $arguments += '-Shortcut' }
  Start-Process powershell -ArgumentList $arguments -WindowStyle Hidden | Out-Null
  $timer.Start()
})

Update-Checks
[void]$form.ShowDialog()
