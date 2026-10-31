# Runs Ruffle's Stage3D visual tests on the PC host and compares each last frame with the reference image.
# Usage: .\s3d-tests.ps1 [-Filter stage3d_*]
# Animated tests may differ only because our frame timing isn't the test harness's exact frame count.
param([string]$Filter = '*')
Add-Type -AssemblyName System.Drawing
$rw = Split-Path $PSScriptRoot
$src = "$rw\ruffle\tests\tests\swfs\avm2"
New-Item -ItemType Directory -Force "$rw\testroot\s3d" | Out-Null

function Pixels([string]$path) {
    $bmp = [Drawing.Bitmap]::FromFile($path)
    $rect = New-Object Drawing.Rectangle 0, 0, $bmp.Width, $bmp.Height
    $data = $bmp.LockBits($rect, 'ReadOnly', [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $bytes = New-Object byte[] ($data.Stride * $bmp.Height)
    [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $bmp.UnlockBits($data)
    $r = @{ W = $bmp.Width; H = $bmp.Height; Stride = $data.Stride; Bytes = $bytes }
    $bmp.Dispose()
    $r
}

$results = foreach ($dir in Get-ChildItem $src -Directory | Where-Object { $_.Name -like $Filter -and ($_.Name -match 'stage3d|context3d') }) {
    $expected = Get-ChildItem $dir.FullName -Filter '*.expected.png' | Select-Object -First 1
    if (-not $expected -or -not (Test-Path "$($dir.FullName)\test.swf")) { continue }
    $toml = Get-Content "$($dir.FullName)\test.toml" -Raw
    $frames = if ($toml -match 'num_frames\s*=\s*(\d+)') { [int]$Matches[1] } elseif ($toml -match 'num_ticks\s*=\s*(\d+)') { [int]$Matches[1] } else { 1 }
    $name = $dir.Name
    Copy-Item "$($dir.FullName)\test.swf" "$rw\testroot\s3d\$name.swf" -Force
    $exp = Pixels $expected.FullName
    $ticks = [Math]::Max(3, $frames * 3 + 2)
    & "$PSScriptRoot\run-pc.ps1" -Movie "s3d/$name.swf" -Name "s3d_$name" -W $exp.W -H $exp.H -Frames ($ticks + 1) -Every $ticks | Out-Null
    $shot = Get-ChildItem "$rw\out\s3d_$name\*.png" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if (-not $shot) { [pscustomobject]@{ Test = $name; Diff = 'no shot'; Errors = '' }; continue }
    $got = Pixels $shot.FullName
    $bad = 0
    for ($i = 0; $i -lt $exp.Bytes.Length; $i += 4) {
        $d = [Math]::Abs($exp.Bytes[$i] - $got.Bytes[$i]) + [Math]::Abs($exp.Bytes[$i + 1] - $got.Bytes[$i + 1]) + [Math]::Abs($exp.Bytes[$i + 2] - $got.Bytes[$i + 2])
        if ($d -gt 24) { $bad++ }
    }
    $pct = [Math]::Round(100.0 * $bad / ($exp.W * $exp.H), 2)
    $errs = (Get-Content "$rw\out\s3d_$name\log.txt" | Select-String 'ERROR|WARN.*Stage3D|panic|trap' | Select-Object -First 2 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(110, $_.Line.Length)) }) -join ' | '
    [pscustomobject]@{ Test = $name; Diff = "$pct%"; Errors = $errs }
}
$results | Format-Table -AutoSize -Wrap | Out-String -Width 220
