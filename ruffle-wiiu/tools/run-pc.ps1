# Runs the PC test host headless on testroot\<Movie> and converts screenshots to PNG.
# Usage: .\run-pc.ps1 -Movie swf/unfair_mario.swf -Name mario [-Frames 600] [-Every 100] [-Script tests\x.txt] [-W 800 -H 600]
param([string]$Movie, [string]$Name, [int]$Frames = 600, [int]$Every = 0, [string]$Script, [int]$W = 800, [int]$H = 600, [switch]$Show, [string]$Spoof)
$rw = Split-Path $PSScriptRoot
$o = "$rw\out\$Name"
New-Item -ItemType Directory -Force $o | Out-Null
# Only old screenshots in this run's own folder are removed.
if ($Name -and (Test-Path $o)) { Get-ChildItem $o -File | Where-Object { $_.Extension -in '.bmp', '.png' } | Remove-Item }
$a = @("$rw\testroot", $Movie, '-w', $W, '-h', $H, '-frames', $Frames, '-out', $o)
if ($Every) { $a += @('-every', $Every) }
if ($Script) { $a += @('-script', (Resolve-Path $Script).Path) }
if ($Show) { $a += '-show' }
if ($Spoof) { $a += @('-spoof', $Spoof) }
$p = Start-Process -FilePath "$rw\build-pc\rwpc.exe" -ArgumentList $a -NoNewWindow -Wait -PassThru -RedirectStandardError "$o\log.txt"
"$Name exit $($p.ExitCode)"
Get-Content "$o\log.txt" | Select-String -NotMatch 'INFO shot ' | Select-Object -Last 12 | ForEach-Object { "  $_" }
Add-Type -AssemblyName System.Drawing
Get-ChildItem "$o\*.bmp" | ForEach-Object {
    $bm = [System.Drawing.Bitmap]::FromFile($_.FullName)
    $bm.Save(($_.FullName -replace '\.bmp$', '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
    $bm.Dispose()
    Remove-Item $_.FullName
}
Get-ChildItem "$o\*.png" | ForEach-Object { "  $($_.Name)" }
