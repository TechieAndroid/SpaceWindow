<#
.SYNOPSIS
    Saves a PNG of a process's main window (used for visual smoke tests).
.DESCRIPTION
    Uses PrintWindow with PW_RENDERFULLCONTENT, which captures DirectComposition
    and DirectX content such as WinUI 3 windows, even when they are covered.
#>
param(
    [Parameter(Mandatory)] [int] $ProcessId,
    [Parameter(Mandatory)] [string] $Path,
    # Copy from the screen instead: includes popup windows (menus, flyouts)
    # but needs the window to be unobstructed.
    [switch] $Screen
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class WindowCapture
{
    public delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr hwnd, uint command);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    public static IntPtr FindMainWindow(uint processId)
    {
        IntPtr found = IntPtr.Zero;
        EnumWindows((hwnd, lParam) =>
        {
            uint pid;
            GetWindowThreadProcessId(hwnd, out pid);
            if (pid == processId && IsWindowVisible(hwnd) && GetWindow(hwnd, 4 /* GW_OWNER */) == IntPtr.Zero)
            {
                found = hwnd;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@

$hwnd = [WindowCapture]::FindMainWindow([uint32]$ProcessId)
if ($hwnd -eq [IntPtr]::Zero) { throw "No visible window for process $ProcessId" }
$rect = New-Object WindowCapture+RECT
[void][WindowCapture]::GetWindowRect($hwnd, [ref]$rect)
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
$bitmap = New-Object System.Drawing.Bitmap $width, $height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
if ($Screen) {
    $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size $width, $height))
    $ok = $true
} else {
    $hdc = $graphics.GetHdc()
    $ok = [WindowCapture]::PrintWindow($hwnd, $hdc, 2)  # PW_RENDERFULLCONTENT
    $graphics.ReleaseHdc($hdc)
}
$graphics.Dispose()
$bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()
"captured ${width}x${height} (PrintWindow=$ok) -> $Path"
