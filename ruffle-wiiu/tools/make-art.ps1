# Draws the Wii U home menu icon and the boot splashes into ruffle-wiiu\host\wiiu\meta\:
#   icon.png (128x128), tv.png (1280x720), drc.png (854x480).
# The logo is original: a lightning bolt on a dark rounded tile with a rainbow edge (Flash-style, not
# Adobe's logo). Usage: .\make-art.ps1
Add-Type -AssemblyName System.Drawing
$out = Join-Path (Split-Path $PSScriptRoot) 'host\wiiu\meta'
New-Item -ItemType Directory -Force $out | Out-Null

$rainbow = @('#FF3B30', '#FF9500', '#FFD60A', '#34C759', '#0A84FF', '#BF5AF2') | ForEach-Object { [Drawing.ColorTranslator]::FromHtml($_) }

function RoundedRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object Drawing.Drawing2D.GraphicsPath
    $p.AddArc($x, $y, 2 * $r, 2 * $r, 180, 90)
    $p.AddArc($x + $w - 2 * $r, $y, 2 * $r, 2 * $r, 270, 90)
    $p.AddArc($x + $w - 2 * $r, $y + $h - 2 * $r, 2 * $r, 2 * $r, 0, 90)
    $p.AddArc($x, $y + $h - 2 * $r, 2 * $r, 2 * $r, 90, 90)
    $p.CloseFigure()
    $p
}

# The logo, size s, top-left (x, y).
function DrawLogo($g, [float]$x, [float]$y, [float]$s) {
    $r = $s * 0.22
    # rainbow edge: a conic-ish sweep made of six pie slices clipped to the tile
    $tile = RoundedRect $x $y $s $s $r
    $g.SetClip($tile)
    for ($i = 0; $i -lt 6; $i++) {
        $b = New-Object Drawing.SolidBrush $rainbow[$i]
        $g.FillPie($b, $x - $s * 0.5, $y - $s * 0.5, $s * 2, $s * 2, -90 + $i * 60, 61)
        $b.Dispose()
    }
    $g.ResetClip()
    # dark inner tile
    $e = $s * 0.07
    $inner = RoundedRect ($x + $e) ($y + $e) ($s - 2 * $e) ($s - 2 * $e) ($r * 0.75)
    $grad = New-Object Drawing.Drawing2D.LinearGradientBrush ([Drawing.PointF]::new($x, $y)), ([Drawing.PointF]::new($x, $y + $s)), ([Drawing.Color]::FromArgb(255, 40, 40, 52)), ([Drawing.Color]::FromArgb(255, 10, 10, 14))
    $g.FillPath($grad, $inner)
    $grad.Dispose()
    # lightning bolt
    $pts = @(
        @(0.58, 0.14), @(0.30, 0.55), @(0.48, 0.55), @(0.40, 0.88), @(0.72, 0.42), @(0.53, 0.42), @(0.62, 0.14)
    ) | ForEach-Object { [Drawing.PointF]::new($x + $_[0] * $s, $y + $_[1] * $s) }
    $shadow = $pts | ForEach-Object { [Drawing.PointF]::new($_.X + $s * 0.025, $_.Y + $s * 0.03) }
    $sb = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(140, 0, 0, 0))
    $g.FillPolygon($sb, [Drawing.PointF[]]$shadow)
    $sb.Dispose()
    $bolt = New-Object Drawing.Drawing2D.LinearGradientBrush ([Drawing.PointF]::new($x, $y + $s * 0.14)), ([Drawing.PointF]::new($x, $y + $s * 0.88)), ([Drawing.Color]::White), ([Drawing.ColorTranslator]::FromHtml('#FFD60A'))
    $g.FillPolygon($bolt, [Drawing.PointF[]]$pts)
    $bolt.Dispose()
}

function NewImage([int]$w, [int]$h) {
    $bmp = New-Object Drawing.Bitmap $w, $h, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.TextRenderingHint = 'AntiAliasGridFit'
    $g.InterpolationMode = 'HighQualityBicubic'
    @($bmp, $g)
}

# Black starry background with a rainbow stripe at the bottom.
function DrawBackground($g, [int]$w, [int]$h) {
    $g.Clear([Drawing.Color]::Black)
    $rnd = New-Object Random 7
    for ($i = 0; $i -lt [int]($w * $h / 4000); $i++) {
        $a = $rnd.Next(60, 200)
        $b = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb($a, 255, 255, 255))
        $d = if ($rnd.Next(8) -eq 0) { 2.5 } else { 1.5 }
        $g.FillEllipse($b, $rnd.Next($w), $rnd.Next($h), $d, $d)
        $b.Dispose()
    }
    $sh = [Math]::Max(4, [int]($h / 90))
    for ($i = 0; $i -lt 6; $i++) {
        $b = New-Object Drawing.SolidBrush $rainbow[$i]
        $g.FillRectangle($b, [float]($w * $i / 6), $h - $sh, [float]($w / 6 + 1), $sh)
        $b.Dispose()
    }
}

# Rainbow "FLASHPLAYER EMU", one color per letter, centered at y.
function DrawTitle($g, [int]$w, [float]$y, [float]$size) {
    $font = New-Object Drawing.Font 'Arial Black', $size, ([Drawing.FontStyle]::Regular), ([Drawing.GraphicsUnit]::Pixel)
    $fmt = [Drawing.StringFormat]::GenericTypographic
    $text = 'FLASHPLAYER EMU'
    $widths = @()
    $total = 0
    foreach ($ch in $text.ToCharArray()) {
        $cw = if ($ch -eq ' ') { $size * 0.35 } else { $g.MeasureString([string]$ch, $font, 1000, $fmt).Width }
        $widths += $cw
        $total += $cw
    }
    $x = ($w - $total) / 2
    $ci = 0
    for ($i = 0; $i -lt $text.Length; $i++) {
        $ch = [string]$text[$i]
        if ($ch -ne ' ') {
            $sb = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(200, 0, 0, 0))
            $g.DrawString($ch, $font, $sb, $x + $size * 0.05, $y + $size * 0.06, $fmt)
            $sb.Dispose()
            $b = New-Object Drawing.SolidBrush $rainbow[$ci % 6]
            $g.DrawString($ch, $font, $b, $x, $y, $fmt)
            $b.Dispose()
            $ci++
        }
        $x += $widths[$i]
    }
    $font.Dispose()
}

function DrawSplash([int]$w, [int]$h, [string]$path) {
    $bmp, $g = NewImage $w $h
    DrawBackground $g $w $h
    $s = $h * 0.36
    DrawLogo $g (($w - $s) / 2) ($h * 0.16) $s
    DrawTitle $g $w ($h * 0.60) ($h * 0.085)
    $font = New-Object Drawing.Font 'Arial', ($h * 0.035), ([Drawing.FontStyle]::Bold), ([Drawing.GraphicsUnit]::Pixel)
    $b = New-Object Drawing.SolidBrush ([Drawing.ColorTranslator]::FromHtml('#FFD60A'))
    $fmt = New-Object Drawing.StringFormat
    $fmt.Alignment = 'Center'
    $g.DrawString('Flash games on your Wii U!', $font, $b, ($w / 2), ($h * 0.76), $fmt)
    $g.Dispose()
    $bmp.Save($path, [Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

# Home menu icon: the logo filling the 128x128 square.
$bmp, $g = NewImage 128 128
$g.Clear([Drawing.Color]::Transparent)
DrawLogo $g 2 2 124
$g.Dispose()
$bmp.Save((Join-Path $out 'icon.png'), [Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

DrawSplash 1280 720 (Join-Path $out 'tv.png')
DrawSplash 854 480 (Join-Path $out 'drc.png')
Get-ChildItem $out | ForEach-Object { "$($_.Name) $($_.Length)" }
