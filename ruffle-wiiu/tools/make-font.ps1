# Renders host/wiiu/fonts/Chewy-Regular.ttf (Apache 2.0, Google Fonts) into host/wiiu/font_chewy.h: for
# ASCII 32..126 an 8-bit coverage mask of each glyph (fill) and of its fat outline (line), in one strip.
# ui.c scales and colours them (yellow letters with a black or red outline, like old Flash game sites).
# Usage: .\make-font.ps1   (build-wiiu.ps1 runs it when the .ttf is newer than the header)
Add-Type -AssemblyName System.Drawing
$rw = Split-Path $PSScriptRoot
$ttf = Join-Path $rw 'host\wiiu\fonts\Chewy-Regular.ttf'
$out = Join-Path $rw 'host\wiiu\font_chewy.h'
$em = 52        # font size in pixels
$h = 76         # cell height
$top = 2        # top margin in the cell
$pad = 5        # outline width (and margin left/right)

$pfc = New-Object Drawing.Text.PrivateFontCollection
$pfc.AddFontFile($ttf)
$family = $pfc.Families[0]
$fmt = [Drawing.StringFormat]::GenericTypographic
$fmt.FormatFlags = $fmt.FormatFlags -bor [Drawing.StringFormatFlags]::MeasureTrailingSpaces

$glyphs = @()
foreach ($code in 32..126) {
    $ch = [string][char]$code
    $path = New-Object Drawing.Drawing2D.GraphicsPath
    $path.AddString($ch, $family, [int][Drawing.FontStyle]::Regular, $em, (New-Object Drawing.PointF 0, $top), $fmt)
    $b = $path.GetBounds()
    # Advance: measured with the typographic format (spaces included).
    $tmp = New-Object Drawing.Bitmap 4, 4
    $g0 = [Drawing.Graphics]::FromImage($tmp)
    $font = New-Object Drawing.Font $family, $em, ([Drawing.FontStyle]::Regular), ([Drawing.GraphicsUnit]::Pixel)
    $adv = [Math]::Ceiling($g0.MeasureString($ch, $font, 1000, $fmt).Width)
    $g0.Dispose(); $tmp.Dispose(); $font.Dispose()
    if ($b.Width -le 0 -or $b.Width -gt 1000) { $left = 0; $w = 1 } else { $left = [int][Math]::Floor($b.X) - $pad; $w = [int][Math]::Ceiling($b.Right) - $left + $pad }
    $adv = [int]$adv
    $bmpF = New-Object Drawing.Bitmap ([int]$w), ([int]$h), ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $bmpL = New-Object Drawing.Bitmap ([int]$w), ([int]$h), ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    foreach ($pair in @(@($bmpF, $false), @($bmpL, $true))) {
        $g = [Drawing.Graphics]::FromImage($pair[0])
        $g.SmoothingMode = 'AntiAlias'
        $g.Clear([Drawing.Color]::Black)
        $g.TranslateTransform(-$left, 0)
        if ($pair[1]) {
            $pen = New-Object Drawing.Pen ([Drawing.Color]::White), ($pad * 2)
            $pen.LineJoin = 'Round'
            $g.DrawPath($pen, $path)
            $pen.Dispose()
        }
        $g.FillPath([Drawing.Brushes]::White, $path)
        $g.Dispose()
    }
    $glyphs += [pscustomobject]@{ Code = $code; Left = $left; W = $w; Adv = $adv; Fill = $bmpF; Line = $bmpL }
    $path.Dispose()
}

$stripW = ($glyphs | Measure-Object -Property W -Sum).Sum
$fill = New-Object byte[] ($stripW * $h)
$line = New-Object byte[] ($stripW * $h)
$x = 0
function CopyRed($bmp, [byte[]]$dst, [int]$x0, [int]$stride) {
    $rect = New-Object Drawing.Rectangle 0, 0, $bmp.Width, $bmp.Height
    $data = $bmp.LockBits($rect, 'ReadOnly', [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $px = New-Object byte[] ($data.Stride * $bmp.Height)
    [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $px, 0, $px.Length)
    $bmp.UnlockBits($data)
    for ($yy = 0; $yy -lt $bmp.Height; $yy++) {
        for ($xx = 0; $xx -lt $bmp.Width; $xx++) { $dst[$yy * $stride + $x0 + $xx] = $px[$yy * $data.Stride + $xx * 4 + 2] }
    }
}
foreach ($gl in $glyphs) {
    $gl | Add-Member X $x
    CopyRed $gl.Fill $fill $x $stripW
    CopyRed $gl.Line $line $x $stripW
    $x += $gl.W
    $gl.Fill.Dispose(); $gl.Line.Dispose()
}

# The eye of a lowercase "a" is filled in the outline mask, so it shows as solid outline colour (yellow in
# the title) instead of a speck of background in the middle of it. Holes reachable from the cell edge are
# outside; enclosed ones up to $maxHole pixels are filled. Only "a": other letters keep their holes.
$maxHole = 400
foreach ($gl in ($glyphs | Where-Object { $_.Code -eq [int][char]'a' })) {
    $w = $gl.W
    $seen = New-Object bool[] ($w * $h)
    $queue = New-Object 'System.Collections.Generic.Queue[int]'
    $isBg = { param($i) $line[[int][Math]::Floor($i / $w) * $stripW + $gl.X + ($i % $w)] -lt 128 }
    for ($i = 0; $i -lt $w * $h; $i++) {
        $xx = $i % $w; $yy = [Math]::Floor($i / $w)
        if (($xx -eq 0 -or $yy -eq 0 -or $xx -eq $w - 1 -or $yy -eq $h - 1) -and (& $isBg $i)) { $seen[$i] = $true; $queue.Enqueue($i) }
    }
    while ($queue.Count) {
        $i = $queue.Dequeue(); $xx = $i % $w; $yy = [Math]::Floor($i / $w)
        foreach ($d in @(@(1, 0), @(-1, 0), @(0, 1), @(0, -1))) {
            $nx = $xx + $d[0]; $ny = $yy + $d[1]
            if ($nx -lt 0 -or $ny -lt 0 -or $nx -ge $w -or $ny -ge $h) { continue }
            $j = $ny * $w + $nx
            if (-not $seen[$j] -and (& $isBg $j)) { $seen[$j] = $true; $queue.Enqueue($j) }
        }
    }
    # Enclosed background pixels, grouped into holes.
    for ($i = 0; $i -lt $w * $h; $i++) {
        if ($seen[$i] -or -not (& $isBg $i)) { continue }
        $hole = New-Object 'System.Collections.Generic.List[int]'
        $seen[$i] = $true; $queue.Enqueue($i)
        while ($queue.Count) {
            $k = $queue.Dequeue(); $hole.Add($k); $xx = $k % $w; $yy = [Math]::Floor($k / $w)
            foreach ($d in @(@(1, 0), @(-1, 0), @(0, 1), @(0, -1))) {
                $nx = $xx + $d[0]; $ny = $yy + $d[1]
                if ($nx -lt 0 -or $ny -lt 0 -or $nx -ge $w -or $ny -ge $h) { continue }
                $j = $ny * $w + $nx
                if (-not $seen[$j] -and (& $isBg $j)) { $seen[$j] = $true; $queue.Enqueue($j) }
            }
        }
        if ($hole.Count -le $maxHole) {
            foreach ($k in $hole) { $line[[int][Math]::Floor($k / $w) * $stripW + $gl.X + ($k % $w)] = 255 }
        }
    }
    # The eye in the fill mask: cleared and grown by a pixel, so it reads as a clean yellow dot instead of
    # a muddy red-yellow speck. (The eye: fill pixels under 200 that can't reach the cell edge through
    # other such pixels.)
    $outside = New-Object bool[] ($w * $h)
    $low = { param($i) $fill[[int][Math]::Floor($i / $w) * $stripW + $gl.X + ($i % $w)] -lt 200 }
    for ($i = 0; $i -lt $w * $h; $i++) {
        $xx = $i % $w; $yy = [Math]::Floor($i / $w)
        if (($xx -eq 0 -or $yy -eq 0 -or $xx -eq $w - 1 -or $yy -eq $h - 1) -and (& $low $i)) { $outside[$i] = $true; $queue.Enqueue($i) }
    }
    while ($queue.Count) {
        $i = $queue.Dequeue(); $xx = $i % $w; $yy = [Math]::Floor($i / $w)
        foreach ($d in @(@(1, 0), @(-1, 0), @(0, 1), @(0, -1))) {
            $nx = $xx + $d[0]; $ny = $yy + $d[1]
            if ($nx -lt 0 -or $ny -lt 0 -or $nx -ge $w -or $ny -ge $h) { continue }
            $j = $ny * $w + $nx
            if (-not $outside[$j] -and (& $low $j)) { $outside[$j] = $true; $queue.Enqueue($j) }
        }
    }
    $eye = @()
    for ($i = 0; $i -lt $w * $h; $i++) { if (-not $outside[$i] -and (& $low $i)) { $eye += $i } }
    foreach ($k in $eye) {
        $xx = $k % $w; $yy = [Math]::Floor($k / $w)
        foreach ($d in @(@(0, 0), @(1, 0), @(-1, 0), @(0, 1), @(0, -1))) {
            $nx = $xx + $d[0]; $ny = $yy + $d[1]
            if ($nx -ge 0 -and $ny -ge 0 -and $nx -lt $w -and $ny -lt $h) {
                $fill[$ny * $stripW + $gl.X + $nx] = 0
                $line[$ny * $stripW + $gl.X + $nx] = 255 # solid yellow underneath
            }
        }
    }
}

$sb = New-Object Text.StringBuilder
[void]$sb.AppendLine('// Generated by tools/make-font.ps1 from fonts/Chewy-Regular.ttf (Apache License 2.0). Do not edit.')
[void]$sb.AppendLine('#pragma once')
[void]$sb.AppendLine('#include <stdint.h>')
[void]$sb.AppendLine("#define CHEWY_EM $em")
[void]$sb.AppendLine("#define CHEWY_H $h")
[void]$sb.AppendLine("#define CHEWY_STRIP_W $stripW")
[void]$sb.AppendLine('// For ASCII 32..126: x in the strip, cell width, offset of the cell from the pen, advance.')
[void]$sb.AppendLine('static const int16_t chewy_glyphs[95][4] = {')
foreach ($gl in $glyphs) { [void]$sb.AppendLine("    { $($gl.X), $($gl.W), $($gl.Left), $($gl.Adv) },") }
[void]$sb.AppendLine('};')
foreach ($pair in @(@('chewy_fill', $fill), @('chewy_line', $line))) {
    [void]$sb.AppendLine("static const uint8_t $($pair[0])[CHEWY_STRIP_W * CHEWY_H] = {")
    $data = $pair[1]
    for ($i = 0; $i -lt $data.Length; $i += 32) {
        $n = [Math]::Min(32, $data.Length - $i)
        [void]$sb.AppendLine(($data[$i..($i + $n - 1)] -join ',') + ',')
    }
    [void]$sb.AppendLine('};')
}
[IO.File]::WriteAllText($out, $sb.ToString(), (New-Object Text.UTF8Encoding $false))
"wrote $out ($stripW x $h strip)"
