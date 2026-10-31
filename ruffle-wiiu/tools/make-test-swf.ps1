# Writes a minimal uncompressed SWF with a plain colored stage, for layout tests (e.g. widescreen vs 4:3).
# Usage: .\make-test-swf.ps1 -Out testroot\swf\wide.swf -W 1280 -H 720 [-Color 34c759]
param([string]$Out, [int]$W = 1280, [int]$H = 720, [string]$Color = '34c759')

# RECT: 5-bit field size, then xmin xmax ymin ymax in twips, packed MSB first.
$bits = New-Object System.Collections.Generic.List[int]
function Put([int]$value, [int]$n) { for ($i = $n - 1; $i -ge 0; $i--) { $bits.Add(($value -shr $i) -band 1) } }
$nb = 17 # enough for 32767 px... in twips up to 65535 (signed)
Put $nb 5
Put 0 $nb; Put ($W * 20) $nb; Put 0 $nb; Put ($H * 20) $nb
while ($bits.Count % 8) { $bits.Add(0) }
$rect = for ($i = 0; $i -lt $bits.Count; $i += 8) { $b = 0; for ($k = 0; $k -lt 8; $k++) { $b = ($b -shl 1) -bor $bits[$i + $k] }; [byte]$b }

$c = 0, 2, 4 | ForEach-Object { [Convert]::ToByte($Color.Substring($_, 2), 16) }
$body = [System.Collections.Generic.List[byte]]::new()
$body.AddRange([byte[]]$rect)
$body.AddRange([byte[]](0x00, 0x1e)) # 30 fps (8.8 fixed)
$body.AddRange([byte[]](0x01, 0x00)) # 1 frame
$body.AddRange([byte[]](0x43, 0x02)) # SetBackgroundColor (tag 9, length 3)
$body.AddRange([byte[]]$c)
$body.AddRange([byte[]](0x40, 0x00)) # ShowFrame
$body.AddRange([byte[]](0x00, 0x00)) # End
$len = 8 + $body.Count
$swf = [System.Collections.Generic.List[byte]]::new()
$swf.AddRange([byte[]](0x46, 0x57, 0x53, 8)) # "FWS", version 8
$swf.AddRange([BitConverter]::GetBytes([uint32]$len))
$swf.AddRange($body)
$path = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path (Split-Path $PSScriptRoot) $Out }
[IO.File]::WriteAllBytes($path, $swf.ToArray())
"wrote $path ($len bytes, ${W}x${H})"
