# Writes tests/kbtest.swf: a 550x400 SWF 8 movie with one empty input text field (100,100)-(400,130) px,
# for testing the system keyboard (tap it: the keyboard should open). No Flash tools needed.
param([string]$Out = (Join-Path (Split-Path $PSScriptRoot) 'tests\kbtest.swf'))

$bits = New-Object System.Collections.Generic.List[int]
function Put-Bits([int]$value, [int]$n) { for ($i = $n - 1; $i -ge 0; $i--) { $bits.Add(($value -shr $i) -band 1) } }
function Take-Bytes {
    while ($bits.Count % 8) { $bits.Add(0) }
    $b = New-Object byte[] ($bits.Count / 8)
    for ($i = 0; $i -lt $b.Length; $i++) { $v = 0; for ($j = 0; $j -lt 8; $j++) { $v = ($v -shl 1) -bor $bits[$i * 8 + $j] }; $b[$i] = $v }
    $bits.Clear()
    , $b
}
function Rect([int[]]$v) { Put-Bits 15 5; foreach ($x in $v) { Put-Bits $x 15 }; Take-Bytes }
function U16([int]$v) { [byte[]]@(($v -band 255), ($v -shr 8)) }
function Str([string]$s) { [Text.Encoding]::ASCII.GetBytes($s) + [byte[]]@(0) }
function Tag([int]$code, [byte[]]$body) {
    if ($body.Length -lt 63) { (U16 (($code -shl 6) -bor $body.Length)) + $body }
    else { (U16 (($code -shl 6) -bor 63)) + [BitConverter]::GetBytes([uint32]$body.Length) + $body }
}

$tags = @()
$tags += Tag 9 ([byte[]]@(255, 255, 255))                                     # SetBackgroundColor
$edit = (U16 1) + (Rect @(2000, 8000, 2000, 2600)) +                          # id 1, bounds (xmin xmax ymin ymax)
        [byte[]]@(0x06, 0x08) +                                               # HasTextColor | HasMaxLength; Border
        [byte[]]@(0, 0, 0, 255) + (U16 20) + (Str '')                         # black, max 20 chars, no variable
$tags += Tag 37 $edit                                                         # DefineEditText
$tags += Tag 26 ([byte[]]@(0x22) + (U16 1) + (U16 1) + (Str 'name'))          # PlaceObject2 id 1 at depth 1, "name"
$tags += Tag 1 ([byte[]]@())                                                  # ShowFrame
$tags += Tag 0 ([byte[]]@())                                                  # End
$body = (Rect @(0, 11000, 0, 8000)) + (U16 (30 * 256)) + (U16 1)              # 550x400, 30 fps, 1 frame
foreach ($t in $tags) { $body += $t }
$file = [Text.Encoding]::ASCII.GetBytes('FWS') + [byte[]]@(8) + [BitConverter]::GetBytes([uint32]($body.Length + 8)) + $body
[IO.File]::WriteAllBytes($Out, [byte[]]$file)
"wrote $Out ($($file.Length) bytes)"
