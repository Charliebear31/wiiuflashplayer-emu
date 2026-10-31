# Runs a scripted test of the Wii U build in Cemu. The game reads the script from the emulated SD card
# (wiiu/apps/pvz/autotest.txt) and saves its own framebuffer screenshots and log.txt there, so nothing
# else on the desktop is captured. Cemu starts minimized and is closed afterwards.
# Usage: .\cemu-test.ps1 -Script tests\wiiu-01-title.txt [-App build-wiiu\SexyAppFramework\LawnProject.wuhb] [-Timeout 180] [-Fresh] [-Mods folder]
#   -Fresh deletes the Wii U save data (new player) first.
#   -Mods copies a folder of test mods to the SD card's wiiu/apps/pvz/mods (replacing what is there).
#   -CemuDir / -Out: another Cemu folder under buildtools\cemu (its own SD card) and shots folder
#   (build-wiiu\shots-<name>), so two test runs can go at the same time.
#   -AppFolder: the game's SD card folder (wiiu/apps/<folder>), e.g. peggle for the Peggle build.
param([string]$Script, [string]$App = "build-wiiu\SexyAppFramework\LawnProject.wuhb", [int]$Timeout = 180, [switch]$Fresh, [string]$Mods,
    [string]$CemuDir = "Cemu_2.6", [string]$Out = "shots", [string]$AppFolder = "pvz", [string]$OnlineCfg, [switch]$OSReport, [uint64]$LogFlag)
#   -OSReport: Cemu writes the game's OSReport lines to its own portable\log.txt for this run. Use it when
#   Cemu itself crashes: the SD card log loses its last lines then, Cemu's log keeps them.
$root = Split-Path $PSScriptRoot
Add-Type -AssemblyName System.Drawing
if ($CemuDir -notmatch '^Cemu_[\w.]+$' -or $Out -notmatch '^shots(-\w+)?$' -or $AppFolder -notmatch '^[a-z0-9]+$') { throw "bad -CemuDir '$CemuDir', -Out '$Out' or -AppFolder '$AppFolder'" }
$cemu = "$root\buildtools\cemu\$CemuDir"
$appDir = "$cemu\portable\sdcard\wiiu\apps\$AppFolder"
$out = "$root\build-wiiu\$Out"
if ([string]::IsNullOrWhiteSpace($root) -or -not $appDir.EndsWith("sdcard\wiiu\apps\$AppFolder") -or -not $out.Contains("build-wiiu\shots")) { throw "bad paths: '$appDir' '$out'" }
New-Item -ItemType Directory -Force $appDir, $out | Out-Null
Get-ChildItem -LiteralPath $out -File | Remove-Item
if ($Fresh) {
    Get-ChildItem -LiteralPath $appDir | Where-Object { $_.Name -ne 'autotest.txt' } | Remove-Item -Recurse -Force
    "cleared Wii U save folder"
} else {
    Remove-Item "$appDir\shots", "$appDir\log.txt" -Recurse -Force -ErrorAction SilentlyContinue
}
# A log left from an earlier run would be read as this run's
if (Test-Path "$appDir\log.txt") { throw "couldn't delete the old $appDir\log.txt (is Cemu still running?)" }
# -OnlineCfg: the game's online.cfg on the emulated SD card (lines separated by ';'), e.g. a local score server
if ($OnlineCfg) { [IO.File]::WriteAllText("$appDir\online.cfg", ($OnlineCfg -replace ';', "`n") + "`n") }
if ($Mods) {
    # Test mods: replace the emulated SD card's mods folder with a copy of $Mods.
    if (-not (Test-Path -LiteralPath $Mods -PathType Container)) { throw "mods folder not found: '$Mods'" }
    $sdMods = "$appDir\mods"
    if (Test-Path -LiteralPath $sdMods) { Remove-Item -LiteralPath $sdMods -Recurse -Force }
    Copy-Item -LiteralPath $Mods $sdMods -Recurse
    "copied test mods to the SD card"
}
Copy-Item (Join-Path $root $Script) "$appDir\autotest.txt" -Force
$hasQuit = (Get-Content "$appDir\autotest.txt") -match '^\d+\s+quit'

# Mute Cemu for the test run (the game's audio still runs; the music mix is dumped to music.raw):
# TV/GamePad volume 0 in Cemu's settings, restored afterwards.
$settings = "$cemu\portable\settings.xml"
$savedSettings = if (Test-Path -LiteralPath $settings) { [IO.File]::ReadAllText($settings) } else { $null }
if ($savedSettings -ne $null) {
    $muted = $savedSettings
    if ($muted -match '<Audio>') {
        $muted = $muted -replace '<TVVolume>\d+</TVVolume>', '<TVVolume>0</TVVolume>' -replace '<PadVolume>\d+</PadVolume>', '<PadVolume>0</PadVolume>'
        if ($muted -notmatch '<TVVolume>') { $muted = $muted -replace '<Audio>', '<Audio><TVVolume>0</TVVolume><PadVolume>0</PadVolume>' }
    } else {
        $muted = $muted -replace '</content>', "    <Audio>`r`n        <TVVolume>0</TVVolume>`r`n        <PadVolume>0</PadVolume>`r`n    </Audio>`r`n</content>"
    }
    if ($OSReport -and -not $LogFlag) { $LogFlag = [uint64]1 -shl 17 }  # Coreinit Logging (OSReport/OSConsole) in Cemu 2.6, found by bisecting
    if ($LogFlag) { $muted = $muted -replace '<logflag>\d+</logflag>', "<logflag>$LogFlag</logflag>" }
    [IO.File]::WriteAllText($settings, $muted)
}

$p = Start-Process "$cemu\Cemu.exe" -ArgumentList "-g `"$((Resolve-Path (Join-Path $root $App)).Path)`"" -WorkingDirectory $cemu -WindowStyle Minimized -PassThru
$sw = [Diagnostics.Stopwatch]::StartNew()
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $Timeout) {
    Start-Sleep -Seconds 2
    if ($hasQuit -and (Test-Path "$appDir\log.txt") -and (Select-String -Path "$appDir\log.txt" -Pattern 'autotest .*: quit' -Quiet)) { Start-Sleep -Seconds 2; break }
    # The game is stuck (white screen): the game's watchdog reports it in the log. End the test.
    if ((Test-Path "$appDir\log.txt") -and (Select-String -Path "$appDir\log.txt" -Pattern 'watchdog: main loop stuck' -Quiet)) { "game stuck (watchdog), ending the test"; Start-Sleep -Seconds 4; break }
    # GTA SA build: an unported function or a fatal error stops the game on an error screen (OSFatal)
    $stop = if (Test-Path "$appDir\log.txt") { Select-String -Path "$appDir\log.txt" -Pattern 'Not ported yet:|\[critical\]' | Select-Object -First 1 }
    # Copy the log first: Cemu loses its last writes to the SD card when it's killed
    if ($stop) { Copy-Item "$appDir\log.txt" "$out\log-at-stop.txt" -Force; "game stopped on an error, ending the test: $($stop.Line)"; Start-Sleep -Seconds 2; break }
    # The game exited (or crashed) but Cemu stays open: no need to wait for the timeout.
    # With a Cemu log mask a hung game can write gigabytes of API calls: stop instead of scanning them
    if ($LogFlag -and -not $OSReport) {
        if ((Get-Item "$cemu\portable\log.txt" -ErrorAction SilentlyContinue).Length -gt 3GB) { "Cemu log passed 3 GB (game stuck in a loop?), ending the test"; break }
        continue
    }
    # (Cemu's own lines only: with -OSReport/-LogFlag the game's lines are in there too, as [OSConsole])
    if ($sw.Elapsed.TotalSeconds -gt 5 -and (Select-String -Path "$cemu\portable\log.txt" -Pattern '^(?!.*\[OSConsole\]).*(terminated the process|Unhandled exception|Crash)' -Quiet -ErrorAction SilentlyContinue)) { "game ended (see Cemu log)"; Start-Sleep -Seconds 8; break }  # let Cemu finish its crash log (PPC stack)
}
if ($p.HasExited) { "Cemu exited by itself after {0:N0}s" -f $sw.Elapsed.TotalSeconds }
else { "stopping Cemu after {0:N0}s" -f $sw.Elapsed.TotalSeconds; Stop-Process $p -Force }
# Cemu can outlive the process we started; stop any left from this Cemu folder so the next run starts clean
Get-Process Cemu -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq "$cemu\Cemu.exe" } | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
Remove-Item "$appDir\autotest.txt" -ErrorAction SilentlyContinue  # so a normal launch doesn't run the test
if ($savedSettings -ne $null) { [IO.File]::WriteAllText($settings, $savedSettings) }  # Cemu's own volume again
if (Test-Path "$appDir\music.raw") { Move-Item "$appDir\music.raw" "$root\build-wiiu\music.raw" -Force; "music dump: build-wiiu\music.raw" }

foreach ($bmp in Get-ChildItem "$appDir\shots" -Filter *.bmp -ErrorAction SilentlyContinue) {
    $img = [Drawing.Image]::FromFile($bmp.FullName)
    $png = Join-Path $out ([IO.Path]::ChangeExtension($bmp.Name, ".png"))
    $img.Save($png, [Drawing.Imaging.ImageFormat]::Png); $img.Dispose()
    "shot: $png"
}
"--- game log (last 30) ---"
Get-Content "$appDir\log.txt" -ErrorAction SilentlyContinue | Select-String -NotMatch 'libpng warning' | Select-Object -Last 30
"--- Cemu log (errors/crash) ---"
Get-Content "$cemu\portable\log.txt" -ErrorAction SilentlyContinue | Select-String 'rror|xception|crash|abort|unhandled|PPC|assert' | Select-Object -Last 15
