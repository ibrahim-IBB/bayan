# make_icon.ps1 - turns a PNG / JPG / BMP into a multi-size Windows .ico (PNG-compressed entries).
# Used by build.bat. Usage: powershell -File make_icon.ps1 -Src icon\icon.png -Dst app_icon.ico
param(
    [Parameter(Mandatory=$true)][string]$Src,
    [Parameter(Mandatory=$true)][string]$Dst
)
Add-Type -AssemblyName System.Drawing

$img = [System.Drawing.Image]::FromFile((Resolve-Path $Src).Path)
$sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
$pngs = New-Object System.Collections.ArrayList

foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    # fit inside the square, keep the aspect ratio, centre it
    $scale = [Math]::Min($s / $img.Width, $s / $img.Height)
    $w = [int][Math]::Round($img.Width * $scale)
    $h = [int][Math]::Round($img.Height * $scale)
    $x = [int][Math]::Floor(($s - $w) / 2)
    $y = [int][Math]::Floor(($s - $h) / 2)
    $g.DrawImage($img, $x, $y, $w, $h)
    $g.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    [void]$pngs.Add($ms.ToArray())
    $ms.Dispose()
    $bmp.Dispose()
}
$img.Dispose()

$fs = [System.IO.File]::Create((Join-Path (Get-Location).Path $Dst))
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([uint16]0)                 # reserved
$bw.Write([uint16]1)                 # type: icon
$bw.Write([uint16]$sizes.Count)      # number of images

$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dim = $sizes[$i]; if ($dim -ge 256) { $dim = 0 }     # 0 means 256 in the ICO format
    $bw.Write([byte]$dim)            # width
    $bw.Write([byte]$dim)            # height
    $bw.Write([byte]0)               # palette colours
    $bw.Write([byte]0)               # reserved
    $bw.Write([uint16]1)             # colour planes
    $bw.Write([uint16]32)            # bits per pixel
    $bw.Write([uint32]$pngs[$i].Length)
    $bw.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write([byte[]]$p) }
$bw.Close()
$fs.Close()
