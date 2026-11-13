param([string]$Output = (Join-Path $PSScriptRoot 'potato.ico'), [string]$Preview = '')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$sizes = @(16, 24, 32, 48, 64, 128, 256)
$images = @()
foreach ($size in $sizes) {
    $bitmap = [Drawing.Bitmap]::new($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bitmap)
    $g.Clear([Drawing.Color]::Transparent)
    $g.ScaleTransform($size / 32.0, $size / 32.0)
    $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::None
    function Polygon([string]$color, [int[]]$coords) {
        $points = for ($i = 0; $i -lt $coords.Length; $i += 2) { [Drawing.Point]::new($coords[$i], $coords[$i+1]) }
        $brush = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml($color))
        try { $g.FillPolygon($brush, [Drawing.Point[]]$points) } finally { $brush.Dispose() }
    }
    function Rect([string]$color, $x, $y, $w, $h) {
        $brush = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml($color))
        try { $g.FillRectangle($brush, [single]$x, [single]$y, [single]$w, [single]$h) } finally { $brush.Dispose() }
    }
    Polygon '#172C34' @(16,1,30,8,30,24,16,31,2,24,2,8)
    Polygon '#79BC4D' @(16,3,28,9,16,15,4,9)
    Polygon '#557F2E' @(4,10,16,16,16,28,4,22)
    Polygon '#386B32' @(16,16,28,10,28,22,16,28)
    Polygon '#9A683F' @(4,13,16,19,16,28,4,22)
    Polygon '#6C482F' @(16,19,28,13,28,22,16,28)
    Polygon '#A8D85A' @(8,7,16,3,20,5,12,9)
    Polygon '#5A963E' @(16,9,23,6,27,8,20,12)
    Rect '#805332' 5 18 2 2
    Rect '#BD8650' 11 22 2 2
    Rect '#8F643D' 25 18 2 2
    Polygon '#3C3526' @(19,19,20,16,24,16,26,18,25,23,21,25,18,23)
    Polygon '#E4B451' @(20,19,21,17,24,17,25,19,24,22,21,24,19,22)
    Rect '#F7D980' 21 18 2 2
    Rect '#B78237' 22 22 1 1
    $g.Dispose()
    $stream = [IO.MemoryStream]::new()
    try {
        $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
        $images += ,$stream.ToArray()
        if ($size -eq 256 -and $Preview) { $bitmap.Save($Preview, [Drawing.Imaging.ImageFormat]::Png) }
    } finally { $stream.Dispose(); $bitmap.Dispose() }
}
$file = [IO.File]::Create($Output)
$writer = [IO.BinaryWriter]::new($file)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $dimension = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
        $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$images[$i].Length); $writer.Write([uint32]$offset)
        $offset += $images[$i].Length
    }
    foreach ($image in $images) { $writer.Write([byte[]]$image) }
} finally { $writer.Dispose() }
Write-Output "Created original seven-resolution icon: $Output"
