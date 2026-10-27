# Downscaled screenshot of the mk9 window, for checking a test run by eye.
# Run inline (the tool shell): . .\tools\snap.ps1; Snap-Mk9 -Out <png> [-Width 1280]
function Snap-Mk9 {
    param([Parameter(Mandatory = $true)][string]$Out, [int]$Width = 1280, [string]$ProcessName = "mk9")
    Add-Type -AssemblyName System.Drawing
    if (-not ("SnapNative" -as [type])) {
        Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class SnapNative {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
    }
    [SnapNative]::SetProcessDPIAware() | Out-Null
    $p = Get-Process -Name $ProcessName -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    $r = New-Object SnapNative+RECT
    [SnapNative]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size $w, $h))
    $sh = [int]($h * $Width / $w)
    $small = New-Object System.Drawing.Bitmap $Width, $sh
    $g2 = [System.Drawing.Graphics]::FromImage($small)
    $g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g2.DrawImage($bmp, 0, 0, $Width, $sh)
    $small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $g2.Dispose(); $bmp.Dispose(); $small.Dispose()
    "saved $Out"
}
