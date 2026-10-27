# A contact sheet of a test run's snapshots (snap-<frame>.png), labelled by name, for checking a
# whole run in one image. Dot-source, then: New-ContactSheet -Dir <folder> -Out <png> [-Columns 4] [-TileWidth 480]
function New-ContactSheet {
    param([Parameter(Mandatory = $true)][string]$Dir, [Parameter(Mandatory = $true)][string]$Out,
          [int]$Columns = 4, [int]$TileWidth = 480)
    Add-Type -AssemblyName System.Drawing
    $files = Get-ChildItem $Dir -Filter "*.png" | Where-Object { $_.FullName -ne $Out } |
        Sort-Object { if ($_.BaseName -match '(\d+)$') { [int]$Matches[1] } else { 0 } }
    if (-not $files) { throw "no snapshots in $Dir" }
    $first = [System.Drawing.Image]::FromFile($files[0].FullName)
    $tileH = [int]($first.Height * $TileWidth / $first.Width)
    $first.Dispose()
    $rows = [math]::Ceiling($files.Count / $Columns)
    $sheet = New-Object System.Drawing.Bitmap ($Columns * $TileWidth), ($rows * ($tileH + 20))
    $g = [System.Drawing.Graphics]::FromImage($sheet)
    $g.Clear([System.Drawing.Color]::Black)
    $font = New-Object System.Drawing.Font "Segoe UI", 10
    $i = 0
    foreach ($f in $files) {
        $img = [System.Drawing.Image]::FromFile($f.FullName)
        $x = ($i % $Columns) * $TileWidth; $y = [math]::Floor($i / $Columns) * ($tileH + 20)
        $g.DrawImage($img, $x, $y + 20, $TileWidth, $tileH)
        $g.DrawString($f.BaseName, $font, [System.Drawing.Brushes]::Yellow, $x + 4, $y + 2)
        $img.Dispose(); $i++
    }
    $sheet.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $sheet.Dispose()
    "saved $Out ($($files.Count) tiles)"
}
