# Regenerates res\windira.ico (PNG-compressed icon, 16-256 px).
# Usage: powershell -ExecutionPolicy Bypass -File tools\make-icon.ps1
Add-Type -AssemblyName System.Drawing

$out = Join-Path $PSScriptRoot '..\res\windira.ico'
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$images = @()

foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.TextRenderingHint = 'AntiAliasGridFit'
    $g.Clear([System.Drawing.Color]::Transparent)

    $r = [Math]::Max(2, [int]($s * 0.22))
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $w = $s - 1
    $path.AddArc(0, 0, $r * 2, $r * 2, 180, 90)
    $path.AddArc($w - $r * 2, 0, $r * 2, $r * 2, 270, 90)
    $path.AddArc($w - $r * 2, $w - $r * 2, $r * 2, $r * 2, 0, 90)
    $path.AddArc(0, $w - $r * 2, $r * 2, $r * 2, 90, 90)
    $path.CloseFigure()
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.Point 0, 0), (New-Object System.Drawing.Point $s, $s),
        [System.Drawing.Color]::FromArgb(255, 37, 99, 235), [System.Drawing.Color]::FromArgb(255, 14, 165, 233))
    $g.FillPath($brush, $path)

    # A white name tag with a punched hole: "label this device".
    $white = [System.Drawing.Brushes]::White
    $tx = $s * 0.20; $ty = $s * 0.30; $tw = $s * 0.62; $th = $s * 0.40
    $tag = New-Object System.Drawing.Drawing2D.GraphicsPath
    $tag.AddPolygon(@(
        (New-Object System.Drawing.PointF ($tx + $th * 0.45), $ty),
        (New-Object System.Drawing.PointF ($tx + $tw), $ty),
        (New-Object System.Drawing.PointF ($tx + $tw), ($ty + $th)),
        (New-Object System.Drawing.PointF ($tx + $th * 0.45), ($ty + $th)),
        (New-Object System.Drawing.PointF $tx, ($ty + $th / 2))))
    $g.FillPath($white, $tag)
    $hole = [Math]::Max(1.5, $s * 0.09)
    $g.FillEllipse($brush, ($tx + $th * 0.42 - $hole / 2), ($ty + $th / 2 - $hole / 2), $hole, $hole)
    if ($s -ge 32) {
        $lineBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 37, 99, 235))
        $lx = $tx + $th * 0.75; $lw = $tw - $th * 0.75 - $s * 0.06
        $g.FillRectangle($lineBrush, $lx, ($ty + $th * 0.30), $lw, [Math]::Max(1, $s * 0.06))
        $g.FillRectangle($lineBrush, $lx, ($ty + $th * 0.60), ($lw * 0.65), [Math]::Max(1, $s * 0.06))
    }
    $g.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $images += , @{ Size = $s; Bytes = $ms.ToArray() }
    $bmp.Dispose()
}

$fs = [System.IO.File]::Create($out)
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($img in $images) {
    $dim = if ($img.Size -ge 256) { 0 } else { $img.Size }
    $bw.Write([Byte]$dim); $bw.Write([Byte]$dim); $bw.Write([Byte]0); $bw.Write([Byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$img.Bytes.Length); $bw.Write([UInt32]$offset)
    $offset += $img.Bytes.Length
}
foreach ($img in $images) { $bw.Write($img.Bytes) }
$bw.Close()
Write-Host "Wrote $out"
