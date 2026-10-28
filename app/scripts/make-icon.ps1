<#
.SYNOPSIS
Draws the application icon and writes src\app.ico.

.DESCRIPTION
The icon is a blue tile with a monitor and a phone in front of it. It is drawn
with GDI+ once per size, so small sizes get thicker lines instead of a blurry
downscale. Sizes below 256 are stored as 32-bit bitmaps and 256 as PNG, which
is the layout every Windows tool accepts.

Run this only when the design changes; src\app.ico is checked in.

.PARAMETER PreviewPng
Optional path for a 256 pixel PNG of the result.
#>
param(
    [string] $PreviewPng
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$icoPath = Join-Path $root 'src\app.ico'
$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256

function New-RoundedRect([single] $x, [single] $y, [single] $w, [single] $h, [single] $r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $path.AddArc($x, $y, $d, $d, 180, 90)
    $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

# All coordinates are on a 256 unit grid and scaled to the target size.
function New-IconBitmap([int] $size) {
    $bitmap = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.Clear([System.Drawing.Color]::Transparent)
        $g.ScaleTransform($size / 256.0, $size / 256.0)

        # Small sizes use the whole square and lines at least 1.5 pixels wide.
        $margin = if ($size -le 32) { 0 } else { 8 }
        $stroke = [Math]::Max(13.0, 24.0 * 16 / $size)

        $tileRect = New-Object System.Drawing.RectangleF $margin, $margin, (256 - 2 * $margin), (256 - 2 * $margin)
        $tile = New-RoundedRect $margin $margin (256 - 2 * $margin) (256 - 2 * $margin) 54
        $top = [System.Drawing.Color]::FromArgb(255, 0x4F, 0x8C, 0xF7)
        $bottom = [System.Drawing.Color]::FromArgb(255, 0x24, 0x48, 0xC4)
        $fill = New-Object System.Drawing.Drawing2D.LinearGradientBrush $tileRect, $top, $bottom, 90.0
        $g.FillPath($fill, $tile)

        $white = [System.Drawing.Color]::White
        $pen = New-Object System.Drawing.Pen $white, $stroke
        $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
        $brush = New-Object System.Drawing.SolidBrush $white

        # Monitor: outlined screen, neck and foot.
        $screen = New-RoundedRect 40 62 150 104 12
        $g.DrawPath($pen, $screen)
        $g.FillRectangle($brush, 104, 166, 22, 26)
        $foot = New-RoundedRect 76 188 78 ($stroke) ($stroke / 2)
        $g.FillPath($brush, $foot)

        # Phone in front of the monitor. A band in the tile color separates the two.
        $gap = New-RoundedRect 140 98 84 126 20
        $gapBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0x33, 0x63, 0xD9))
        $g.FillPath($gapBrush, $gap)
        $phone = New-RoundedRect 152 110 60 102 12
        $g.FillPath($brush, $phone)
        if ($size -ge 40) {
            # The phone's own screen, only where there are enough pixels to show it.
            $glass = New-RoundedRect 161 122 42 70 5
            $g.FillPath($gapBrush, $glass)
        }
    } finally {
        $g.Dispose()
    }
    return $bitmap
}

function Get-PngBytes([System.Drawing.Bitmap] $bitmap) {
    $stream = New-Object System.IO.MemoryStream
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    return , $stream.ToArray()
}

# An icon bitmap is a BITMAPINFOHEADER with doubled height, the pixels bottom-up
# as BGRA, then a 1 bit mask that is all zero because the alpha channel is used.
function Get-DibBytes([System.Drawing.Bitmap] $bitmap) {
    $size = $bitmap.Width
    $rect = New-Object System.Drawing.Rectangle 0, 0, $size, $size
    $data = $bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        $pixels = New-Object byte[] ($size * $size * 4)
        for ($row = 0; $row -lt $size; $row++) {
            $source = [IntPtr]::Add($data.Scan0, $row * $data.Stride)
            [System.Runtime.InteropServices.Marshal]::Copy($source, $pixels, ($size - 1 - $row) * $size * 4, $size * 4)
        }
    } finally {
        $bitmap.UnlockBits($data)
    }
    $maskRow = [int] ([Math]::Ceiling($size / 32.0) * 4)
    $stream = New-Object System.IO.MemoryStream
    $writer = New-Object System.IO.BinaryWriter $stream
    $writer.Write([int] 40); $writer.Write([int] $size); $writer.Write([int] ($size * 2))
    $writer.Write([int16] 1); $writer.Write([int16] 32); $writer.Write([int] 0)
    $writer.Write([int] ($pixels.Length + $maskRow * $size))
    $writer.Write([int] 0); $writer.Write([int] 0); $writer.Write([int] 0); $writer.Write([int] 0)
    $writer.Write($pixels)
    $writer.Write((New-Object byte[] ($maskRow * $size)))
    $writer.Flush()
    return , $stream.ToArray()
}

$images = foreach ($size in $sizes) {
    $bitmap = New-IconBitmap $size
    try {
        if ($size -eq 256 -and $PreviewPng) { $bitmap.Save($PreviewPng, [System.Drawing.Imaging.ImageFormat]::Png) }
        [byte[]] $bytes = if ($size -eq 256) { Get-PngBytes $bitmap } else { Get-DibBytes $bitmap }
        [pscustomobject] @{ Size = $size; Bytes = $bytes }
    } finally {
        $bitmap.Dispose()
    }
}

$out = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter $out
$writer.Write([int16] 0); $writer.Write([int16] 1); $writer.Write([int16] $images.Count)
$offset = 6 + 16 * $images.Count
foreach ($image in $images) {
    $dimension = if ($image.Size -eq 256) { 0 } else { $image.Size }   # 0 means 256
    $writer.Write([byte] $dimension); $writer.Write([byte] $dimension)
    $writer.Write([byte] 0); $writer.Write([byte] 0)
    $writer.Write([int16] 1); $writer.Write([int16] 32)
    $writer.Write([int] $image.Bytes.Length); $writer.Write([int] $offset)
    $offset += $image.Bytes.Length
}
foreach ($image in $images) { $writer.Write([byte[]] $image.Bytes) }
$writer.Flush()
[System.IO.File]::WriteAllBytes($icoPath, $out.ToArray())

Write-Host "Wrote $icoPath ($($out.Length) bytes, sizes: $($sizes -join ', '))"
