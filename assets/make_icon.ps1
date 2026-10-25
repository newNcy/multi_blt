# 绘制 DualBlt 图标：左侧单线，柔和曲线两级分叉（1->2->4），右侧四个箭头
# 输出: assets\icon_1024.png + assets\DualBlt.ico（多尺寸）
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$lime = [System.Drawing.Color]::FromArgb(255, 0xD0, 0xFF, 0x00)
$bg   = [System.Drawing.Color]::FromArgb(255, 0x07, 0x08, 0x08)

function New-IconBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    # 透明背景：不做 Clear，让标题栏/任务栏自带底色透出来
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    return @($bmp, $g)
}

function Add-Stroke([System.Drawing.Graphics]$g, [System.Drawing.Drawing2D.GraphicsPath]$p,
                    [System.Drawing.Color]$color, [float]$w) {
    $pen = New-Object System.Drawing.Pen($color, $w)
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $g.DrawPath($pen, $p)
    $pen.Dispose()
}

# 归一化坐标（基于 1024 设计稿），size 缩放
function Build-Path([float]$s) {
    $X = { param($v) [float]($v * $s / 1024.0) }
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath

    # 左侧竖线 + 水平主干（到从左 2/5 处）
    $path.StartFigure()
    $path.AddLine((& $X 130), (& $X 220), (& $X 130), (& $X 804))
    $path.StartFigure()
    $path.AddLine((& $X 130), (& $X 512), (& $X 430), (& $X 512))

    # 2/5 处分叉：上/下支沿竖直切向出发再弯向右，中支直行
    $path.StartFigure()
    $path.AddBezier((& $X 430), (& $X 512), (& $X 430), (& $X 340), (& $X 520), (& $X 240), (& $X 880), (& $X 240))
    $path.StartFigure()
    $path.AddBezier((& $X 430), (& $X 512), (& $X 640), (& $X 512), (& $X 720), (& $X 512), (& $X 880), (& $X 512))
    $path.StartFigure()
    $path.AddBezier((& $X 430), (& $X 512), (& $X 430), (& $X 684), (& $X 520), (& $X 784), (& $X 880), (& $X 784))
    return $path
}

function Add-Arrowheads([System.Drawing.Drawing2D.GraphicsPath]$p, [float]$s) {
    $sc = $s / 1024.0
    $ah = 52 * $sc   # 箭头半高
    $al = 60 * $sc   # 箭头长度
    foreach ($y0 in 320, 704) {
        $y   = $y0 * $sc
        $tip = 950 * $sc
        $bx  = $tip - $al
        $yt  = $y - $ah
        $yb  = $y + $ah
        $p.StartFigure()
        $p.AddLine($tip, $y, $bx, $yt)
        $p.AddLine($bx, $yt, $bx, $yb)
        $p.CloseFigure()
    }
}

# ---- 1024 PNG ----
$bmp, $g = New-IconBitmap 1024
$path = Build-Path 1024
Add-Stroke $g $path $lime 30
$path.Dispose(); $g.Dispose()
$bmp.Save("G:\multi_blt\assets\icon_1024.png", [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

# ---- 多尺寸 ICO（PNG 条目，Vista+ 支持）----
$sizes = 256, 64, 48, 32, 16
$pngs = @{}
foreach ($sz in $sizes) {
    $b, $gg = New-IconBitmap $sz
    $p = Build-Path $sz
    Add-Stroke $gg $p $lime ([Math]::Max(4, [int](30 * $sz / 1024)))
    $p.Dispose(); $gg.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs[$sz] = $ms.ToArray()
    $b.Dispose(); $ms.Dispose()
}

# ICO 打包
$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($out)
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)   # ICONDIR
$offset = 6 + 16 * $sizes.Count
foreach ($sz in $sizes) {
    $d = $pngs[$sz]
    $w = if ($sz -ge 256) { 0 } else { $sz }
    $bw.Write([byte]$w); $bw.Write([byte]$w)          # 宽/高（256 写 0）
    $bw.Write([byte]0); $bw.Write([byte]0)            # 调色板/保留
    $bw.Write([uint16]1); $bw.Write([uint16]32)       # 平面/位深
    $bw.Write([uint32]$d.Length); $bw.Write([uint32]$offset)
    $offset += $d.Length
}
foreach ($sz in $sizes) { $bw.Write($pngs[$sz]) }
$bw.Flush()
[System.IO.File]::WriteAllBytes("G:\multi_blt\assets\DualBlt.ico", $out.ToArray())
$bw.Dispose(); $out.Dispose()
Write-Output "done: icon_1024.png + DualBlt.ico"
