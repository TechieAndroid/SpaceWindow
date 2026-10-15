<#
.SYNOPSIS
    Minimal UI driver for smoke-testing SpaceWindow: mouse and keyboard input
    relative to a process's main window.  Dot-source it, then call the functions.
#>

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class UiDriver
{
    public delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr hwnd, uint command);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    public static IntPtr FindMainWindow(uint processId)
    {
        IntPtr found = IntPtr.Zero;
        EnumWindows((hwnd, lParam) =>
        {
            uint pid;
            GetWindowThreadProcessId(hwnd, out pid);
            if (pid == processId && IsWindowVisible(hwnd) && GetWindow(hwnd, 4) == IntPtr.Zero) { found = hwnd; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@

function Get-WindowOrigin([int] $ProcessId) {
    $hwnd = [UiDriver]::FindMainWindow([uint32]$ProcessId)
    $rect = New-Object UiDriver+RECT
    [void][UiDriver]::GetWindowRect($hwnd, [ref]$rect)
    [void][UiDriver]::SetForegroundWindow($hwnd)
    return $rect
}

function Move-Pointer([int] $ProcessId, [int] $X, [int] $Y) {
    # Inject a real absolute mouse move: SetCursorPos alone moves the cursor
    # without producing the pointer input WinUI listens for.
    $r = Get-WindowOrigin $ProcessId
    Add-Type -AssemblyName System.Windows.Forms
    $screen = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $nx = [int](([double]($r.Left + $X - $screen.Left) * 65535) / ($screen.Width - 1))
    $ny = [int](([double]($r.Top + $Y - $screen.Top) * 65535) / ($screen.Height - 1))
    # MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK
    [UiDriver]::mouse_event(0x0001 -bor 0x8000 -bor 0x4000, $nx, $ny, 0, [UIntPtr]::Zero)
}

function Send-Click([int] $ProcessId, [int] $X, [int] $Y, [switch] $Right, [switch] $Double) {
    Move-Pointer $ProcessId $X $Y
    Start-Sleep -Milliseconds 60
    $down = if ($Right) { 0x0008 } else { 0x0002 }
    $up = if ($Right) { 0x0010 } else { 0x0004 }
    $count = if ($Double) { 2 } else { 1 }
    for ($i = 0; $i -lt $count; $i++) {
        [UiDriver]::mouse_event($down, 0, 0, 0, [UIntPtr]::Zero)
        [UiDriver]::mouse_event($up, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 40
    }
}

function Send-Key([byte] $VirtualKey, [switch] $Alt, [switch] $Ctrl, [switch] $Shift) {
    if ($Ctrl) { [UiDriver]::keybd_event(0x11, 0, 0, [UIntPtr]::Zero) }
    if ($Shift) { [UiDriver]::keybd_event(0x10, 0, 0, [UIntPtr]::Zero) }
    if ($Alt) { [UiDriver]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero) }
    [UiDriver]::keybd_event($VirtualKey, 0, 0, [UIntPtr]::Zero)
    [UiDriver]::keybd_event($VirtualKey, 0, 2, [UIntPtr]::Zero)
    if ($Alt) { [UiDriver]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero) }
    if ($Shift) { [UiDriver]::keybd_event(0x10, 0, 2, [UIntPtr]::Zero) }
    if ($Ctrl) { [UiDriver]::keybd_event(0x11, 0, 2, [UIntPtr]::Zero) }
}
