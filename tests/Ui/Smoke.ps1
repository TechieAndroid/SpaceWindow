<#
.SYNOPSIS
    Smoke-tests the SpaceWindow app: it starts, scans, draws each view,
    opens Settings and About, plays a background animation, and closes.

.DESCRIPTION
    Sends no keyboard or mouse input: the app is driven through UI Automation
    (buttons invoked, toggles toggled) and seen through captures of its
    window (PrintWindow, which works while other windows cover it), so it can
    run while someone uses the PC.

    The app's settings (HKCU\Software\SpaceWindow) are saved first and put
    back afterwards: the test sets a known language, view and animation.  It
    scans a small folder it makes under %TEMP% and deletes again.

.PARAMETER Exe
    The app to test; by default the Release x64 build.

.PARAMETER Shots
    Where the captures go; by default a folder under %TEMP%.

.EXAMPLE
    .\tests\Ui\Smoke.ps1
.EXAMPLE
    .\tests\Ui\Smoke.ps1 -Exe ..\somewhere\SpaceWindow.exe -Shots .\shots
#>
param(
    [string] $Exe = (Join-Path $PSScriptRoot '..\..\bin\x64\Release\SpaceWindow\SpaceWindow.exe'),
    [string] $Shots = (Join-Path $env:TEMP 'SpaceWindowSmoke-shots')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Exe = (Resolve-Path $Exe).Path
$capture = Join-Path $PSScriptRoot '..\..\tools\Capture-Window.ps1'
New-Item -ItemType Directory -Force $Shots | Out-Null
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing

$failures = 0
function Report([bool] $ok, [string] $what) {
    if ($ok) { Write-Host "  [pass] $what" } else { Write-Host "  [FAIL] $what" -ForegroundColor Red; $script:failures++ }
}

# ---------------------------------------------------------------- helpers

function Find-Window([int] $processId) {
    $condition = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::ProcessIdProperty, $processId)
    [Windows.Automation.AutomationElement]::RootElement.FindFirst([Windows.Automation.TreeScope]::Children, $condition)
}

function Find-Element($root, [string] $id) {
    $condition = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::AutomationIdProperty, $id)
    $root.FindFirst([Windows.Automation.TreeScope]::Descendants, $condition)
}

# A toolbar button: in the bar, or in its overflow menu when the window is narrow.
function Find-Button($window, [string] $id) {
    $button = Find-Element $window $id
    if (-not $button) {
        $more = Find-Element $window 'MoreButton'
        if ($more) {
            $more.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke()
            Start-Sleep -Milliseconds 600
            $button = Find-Element $window $id
        }
    }
    $button
}

function Invoke-Button($window, [string] $id) {
    $button = Find-Button $window $id
    if (-not $button) { return $false }
    $button.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke()
    $true
}

function Switch-Toggle($window, [string] $id) {
    $button = Find-Button $window $id
    if (-not $button) { return $false }
    $button.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Toggle()
    $true
}

function Wait-Until([scriptblock] $condition, [int] $milliseconds = 30000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $milliseconds) {
        if (& $condition) { return $true }
        Start-Sleep -Milliseconds 100
    }
    $false
}

function Save-Capture([int] $processId, [string] $name) {
    $path = Join-Path $Shots "$name.png"
    & $capture -ProcessId $processId -Path $path | Out-Null
    $path
}

# How many different colours a grid of points over the middle of a capture finds.
function Measure-Colours([string] $path) {
    $bitmap = [System.Drawing.Bitmap]::FromFile($path)
    try {
        $colours = @{}
        for ($y = [int]($bitmap.Height * 0.3); $y -lt $bitmap.Height * 0.85; $y += 7) {
            for ($x = [int]($bitmap.Width * 0.1); $x -lt $bitmap.Width * 0.9; $x += 7) {
                $colours[$bitmap.GetPixel($x, $y).ToArgb()] = $true
            }
        }
        $colours.Count
    }
    finally { $bitmap.Dispose() }
}

# The share of a grid of points two captures differ at.
function Compare-Captures([string] $a, [string] $b) {
    $one = [System.Drawing.Bitmap]::FromFile($a)
    $two = [System.Drawing.Bitmap]::FromFile($b)
    try {
        if ($one.Width -ne $two.Width -or $one.Height -ne $two.Height) { return 1.0 }
        $points = 0
        $different = 0
        for ($y = [int]($one.Height * 0.2); $y -lt $one.Height * 0.9; $y += 9) {
            for ($x = 8; $x -lt $one.Width - 8; $x += 9) {
                $points++
                if ($one.GetPixel($x, $y).ToArgb() -ne $two.GetPixel($x, $y).ToArgb()) { $different++ }
            }
        }
        $different / [Math]::Max(1, $points)
    }
    finally { $one.Dispose(); $two.Dispose() }
}

function Start-App([string] $folder, [int] $view, [int] $animation) {
    $key = 'HKCU\Software\SpaceWindow'
    & reg.exe add $key /v lang /t REG_SZ /d us /f | Out-Null
    & reg.exe add $key /v view_mode /t REG_DWORD /d $view /f | Out-Null
    & reg.exe add $key /v background_animation /t REG_DWORD /d $animation /f | Out-Null
    & reg.exe add $key /v window_maximized /t REG_DWORD /d 0 /f | Out-Null
    $process = Start-Process -FilePath $Exe -ArgumentList "`"$folder`"" -PassThru
    # Scanned once the title names the folder and its size.
    $leaf = Split-Path $folder -Leaf
    $ready = Wait-Until {
        $process.Refresh()
        $process.MainWindowTitle -like "*$leaf*Total*"
    }
    Start-Sleep -Milliseconds 1000  # the first frames
    [pscustomobject]@{ Process = $process; Ready = $ready }
}

function Stop-App($app) {
    if (-not $app -or $app.Process.HasExited) { return }
    [void]$app.Process.CloseMainWindow()  # a window message, not input
    if (-not $app.Process.WaitForExit(10000)) { Stop-Process -Id $app.Process.Id -Force }
}

# ---------------------------------------------------------------- the test

$backup = Join-Path $env:TEMP "SpaceWindowSmoke-settings-$PID.reg"
$hadSettings = Test-Path 'HKCU:\Software\SpaceWindow'
if ($hadSettings) { & reg.exe export 'HKCU\Software\SpaceWindow' $backup /y | Out-Null }
$folder = Join-Path $env:TEMP "SpaceWindowSmoke-$PID"
$app = $null
try {
    # A small folder to scan, the same every time.
    foreach ($name in 'Photos', 'Photos\2025', 'Music', 'Documents', 'Documents\Old') {
        New-Item -ItemType Directory -Force (Join-Path $folder $name) | Out-Null
    }
    $sizes = @{ 'Photos\2025\a.jpg' = 3MB; 'Photos\b.jpg' = 1MB; 'Music\c.flac' = 5MB; 'Documents\d.pdf' = 600KB;
                'Documents\Old\e.docx' = 200KB; 'f.iso' = 2MB }
    foreach ($entry in $sizes.GetEnumerator()) {
        $stream = [IO.File]::Create((Join-Path $folder $entry.Key))
        $stream.SetLength($entry.Value)
        $stream.Dispose()
    }

    Write-Host "Smoke-testing $Exe"
    $app = Start-App $folder 0 0
    Report $app.Ready 'starts and scans a folder'
    $window = Find-Window $app.Process.Id
    Report ($null -ne $window) 'its window is found'

    $treemap = Save-Capture $app.Process.Id 'treemap'
    Report ((Measure-Colours $treemap) -ge 4) 'draws the treemap'

    Report (Switch-Toggle $window 'PieChartButton') 'Pie Chart button found'
    Start-Sleep -Milliseconds 1200
    $pie = Save-Capture $app.Process.Id 'pie'
    Report ((Compare-Captures $treemap $pie) -gt 0.05) 'switches to the pie chart'

    Report (Switch-Toggle $window 'InvertedPieButton') 'Inverted Pie button found'
    Start-Sleep -Milliseconds 1200
    $inverted = Save-Capture $app.Process.Id 'inverted-pie'
    Report ((Compare-Captures $pie $inverted) -gt 0.02) 'switches to the inverted pie chart'

    [void](Switch-Toggle $window 'InvertedPieButton')
    Start-Sleep -Milliseconds 1200
    $back = Save-Capture $app.Process.Id 'treemap-again'
    Report ((Compare-Captures $treemap $back) -lt 0.02) 'switches back to the treemap'

    Report (Invoke-Button $window 'SettingsButton') 'Setup button found'
    $settings = $null
    $opened = Wait-Until { $script:settings = Find-Element $window 'SettingsContent'; $null -ne $script:settings } 5000
    Report $opened 'opens Settings'
    if ($opened) {
        # Each shortcut's action is named "keys: what it does" for screen readers
        # (the list itself is a plain grid, which UI Automation does not show).
        $condition = New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::ControlTypeProperty, [Windows.Automation.ControlType]::Text)
        $named = @($settings.FindAll([Windows.Automation.TreeScope]::Descendants, $condition) | Where-Object { $_.Current.Name -match '^\S+( / \S+)*: \S' })
        Report ($named.Count -eq 18) "lists the keyboard shortcuts ($($named.Count))"
        [void](Save-Capture $app.Process.Id 'settings')
        Report (Invoke-Button $window 'CloseButton') 'closes Settings'
        Start-Sleep -Milliseconds 800
    }

    Report (Invoke-Button $window 'AboutButton') 'About button found'
    $version = $null
    Report (Wait-Until { $script:version = Find-Element $window 'AboutVersion'; $null -ne $script:version } 5000) 'opens About'
    if ($version) {
        Report ($version.Current.Name -like 'SpaceWindow *') "About names the app ($($version.Current.Name))"
        [void](Invoke-Button $window 'CloseButton')
        Start-Sleep -Milliseconds 800
    }
    Stop-App $app
    Report ($app.Process.HasExited) 'closes'

    # A background animation around the pie chart moves from frame to frame.
    $app = Start-App $folder 1 1
    Report $app.Ready 'starts again with the pie chart and the fire'
    $one = Save-Capture $app.Process.Id 'fire-1'
    Start-Sleep -Milliseconds 400
    $two = Save-Capture $app.Process.Id 'fire-2'
    Report ((Compare-Captures $one $two) -gt 0.01) 'the background animation plays'
    Stop-App $app
    $app = $null
}
finally {
    Stop-App $app
    & reg.exe delete 'HKCU\Software\SpaceWindow' /f 2>$null | Out-Null
    if ($hadSettings) {
        & reg.exe import $backup 2>$null | Out-Null
        Remove-Item $backup -ErrorAction SilentlyContinue
    }
    Remove-Item -Recurse -Force $folder -ErrorAction SilentlyContinue
}

Write-Host "Captures: $Shots"
if ($failures -ne 0) {
    Write-Host "$failures check(s) failed" -ForegroundColor Red
    exit 1
}
Write-Host 'All checks passed'
exit 0
