<#
.SYNOPSIS
    Runs clang-tidy (from the Visual Studio LLVM component) over SpaceWindow's
    hand-written sources, using the checks in .clang-tidy.
.DESCRIPTION
    MSBuild does not produce a compilation database, so this script loads the
    Visual Studio developer environment (for the MSVC and Windows SDK include
    paths) and passes clang-cl style flags that mirror the project settings.
    Run a Release x64 build first: the app's sources include headers that the
    XAML compiler and C++/WinRT generate under obj\.
.OUTPUTS
    The findings, and exit code 1 if there are any.
#>
param(
    [string] $Root = (Resolve-Path "$PSScriptRoot\.."),
    [switch] $IncludeApp = $true
)

$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath
if (-not $vs) { throw 'Visual Studio with the "C++ Clang tools" component is required.' }
$tidy = Join-Path $vs 'VC\Tools\Llvm\x64\bin\clang-tidy.exe'

# Import INCLUDE/LIB/PATH from the developer command prompt (which itself needs vswhere on PATH).
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
$devCmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
cmd /c "`"$devCmd`" -arch=amd64 -host_arch=amd64 -no_logo && set" | ForEach-Object {
    if ($_ -match '^(INCLUDE|LIB|PATH)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] }
}

$packages = Join-Path $Root 'packages'
$wil = Get-ChildItem $packages -Directory -Filter 'Microsoft.Windows.ImplementationLibrary.*' | Select-Object -First 1
$common = @(
    '--driver-mode=cl', '/std:c++20', '/EHsc', '/permissive-', '/Zc:__cplusplus', '/utf-8',
    '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/DSTRICT', '/D_WIN32_WINNT=0x0A00', '/DWINVER=0x0A00',
    '/DUNICODE', '/D_UNICODE', '/DNDEBUG',
    "/imsvc$(Join-Path $wil.FullName 'include')",
    '-Wno-unknown-pragmas', '-Wno-unused-command-line-argument'
)

$jobs = @()
foreach ($dir in 'src\Core', 'tests\CoreTests') {
    Get-ChildItem (Join-Path $Root $dir) -Filter *.cpp | Where-Object Name -ne 'pch.cpp' | ForEach-Object {
        $jobs += [pscustomobject]@{ File = $_.FullName; Flags = $common + @("/I$(Join-Path $Root $dir)") }
    }
}
if ($IncludeApp) {
    $generated = Join-Path $Root 'obj\x64\Release\Generated Files\SpaceWindow'
    if (-not (Test-Path $generated)) { throw "Build Release|x64 first: $generated is missing." }
    $appIncludes = @("/I$(Join-Path $Root 'src\App')", "/imsvc$generated")
    foreach ($package in 'Microsoft.WindowsAppSDK.WinUI.*', 'Microsoft.WindowsAppSDK.InteractiveExperiences.*', 'Microsoft.WindowsAppSDK.Foundation.*') {
        $p = Get-ChildItem $packages -Directory -Filter $package | Select-Object -First 1
        $appIncludes += "/imsvc$(Join-Path $p.FullName 'include')"
    }
    $appDefines = @('/DSPACEWINDOW_WASDK_MAJOR=2', '/DSPACEWINDOW_WASDK_MINOR=5', '/DSPACEWINDOW_WASDK_PATCH=1',
                    '/DWINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP', '-Wno-deprecated-declarations')
    foreach ($name in 'ShellOps.cpp', 'ChromeTheme.cpp', 'MapRenderer.cpp', 'MapRendererPie.cpp', 'MainWindow.xaml.cpp', 'App.xaml.cpp') {
        $jobs += [pscustomobject]@{ File = Join-Path $Root "src\App\$name"; Flags = $common + $appIncludes + $appDefines }
    }
}

# No --config-file: clang-tidy picks the nearest .clang-tidy (tests\ refines the root one).
# Its "N warnings generated." lines go to stderr, hence 2>&1 and "$_" to keep plain text.
if ($PSVersionTable.PSVersion.Major -ge 7) {
    $results = $jobs | ForEach-Object -ThrottleLimit ([Environment]::ProcessorCount) -Parallel {
        $output = & $using:tidy --quiet $_.File -- @($_.Flags) 2>&1 | ForEach-Object { "$_" }
        [pscustomobject]@{ File = $_.File; Output = ($output -join "`n") }
    }
}
else {
    # Windows PowerShell 5.1 has no -Parallel, and stops on redirected stderr under 'Stop'.
    $ErrorActionPreference = 'Continue'
    $results = foreach ($job in $jobs) {
        $output = & $tidy --quiet $job.File -- @($job.Flags) 2>&1 | ForEach-Object { "$_" }
        [pscustomobject]@{ File = $job.File; Output = ($output -join "`n") }
    }
    $ErrorActionPreference = 'Stop'
}

$findings = 0
foreach ($r in $results) {
    $all = $r.Output -split "`r?`n"
    # Warnings count only in our own files; a compile error anywhere (even in an
    # SDK header) means the file was not analyzed, so it always counts.
    $lines = @($all | Where-Object {
        ($_ -match ': warning: ' -and $_ -notmatch '\\(packages|Generated Files)\\' -and $_ -notmatch 'Windows Kits|MSVC\\') -or
        $_ -match ': (error|fatal error): '
    })
    $name = Split-Path $r.File -Leaf
    "{0,-26} {1} finding(s)" -f $name, $lines.Count
    $lines | ForEach-Object { '    ' + ($_ -replace [regex]::Escape("$Root\"), '') }
    $findings += $lines.Count
}
"clang-tidy: $findings finding(s) in $($jobs.Count) files"
exit ([int]($findings -gt 0))
