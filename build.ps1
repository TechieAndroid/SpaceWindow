<#
.SYNOPSIS
    Builds, tests, checks and packages SpaceWindow.

.DESCRIPTION
    1. Finds Visual Studio 2026 (or Build Tools) with the C++ WinUI components.
    2. Restores the NuGet packages listed in the packages.config files.
    3. Optionally runs MSVC code analysis and clang-tidy, and checks the
       sources' formatting against .clang-format.
    4. Builds the core library, the unit tests and the WinUI app.
    5. Runs the unit tests (when the target platform matches this machine),
       and optionally runs them again under AddressSanitizer.
    6. Publishes a ready-to-run folder (and .zip) under artifacts\.

    Works in Windows PowerShell 5.1 and PowerShell 7.

.EXAMPLE
    .\build.ps1                    # Release x64: build, test, publish
.EXAMPLE
    .\build.ps1 -All               # ... plus code analysis, clang-tidy, clang-format and ASan
.EXAMPLE
    .\build.ps1 -Platform ARM64    # cross-compile for Windows on Arm
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'ARM64')] [string] $Platform = 'x64',
    [ValidateSet('Release', 'Debug')] [string] $Configuration = 'Release',
    [switch] $Analyze,     # MSVC /analyze, clang-tidy and clang-format; fails on any finding
    [switch] $Sanitize,    # build and run the unit tests under AddressSanitizer (x64)
    [switch] $NoPublish,
    [switch] $All
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = $PSScriptRoot
$version = '1.0.0'  # keep in step with src\App\SpaceWindow.rc, app.manifest and kVersion in MainWindowShared.h
if ($All) { $Analyze = $true; $Sanitize = $true }

function Step([string] $message) { Write-Host "`n==> $message" -ForegroundColor Cyan }

function Invoke-MSBuild([string] $project, [string] $configuration, [string] $platform, [string[]] $extra = @()) {
    & $script:msbuild $project "/p:Configuration=$configuration" "/p:Platform=$platform" /m /nologo /v:minimal `
        "/clp:ErrorsOnly;WarningsOnly;Summary" @extra
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $project ($configuration|$platform)" }
}

# ---------------------------------------------------------------- toolchain
Step 'Locating Visual Studio'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw 'Visual Studio 2026 or its Build Tools are not installed (see README.md).' }
$vs = & $vswhere -latest -products * -version '[18.0,' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio 2026 (or its Build Tools) with "Desktop development with C++" is required (see README.md).' }
# C++ WinUI projects use the "Windows Store" application type; the IDE's WinUI
# workload and the Build Tools' WinUI build tools both install it.
$appType = Join-Path $vs "MSBuild\Microsoft\VC\v180\Application Type\Windows Store\10.0\Platforms\$Platform"
if (-not (Test-Path $appType)) { throw "The C++ WinUI build tools for $Platform are missing from $vs (see README.md)." }
$script:msbuild = Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'
Write-Host "    $vs"

# ---------------------------------------------------------------- packages
Step 'Restoring NuGet packages'
$nuget = Join-Path $root 'tools\.nuget\nuget.exe'
if (-not (Test-Path $nuget)) {
    New-Item -ItemType Directory -Force (Split-Path $nuget) | Out-Null
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest 'https://dist.nuget.org/win-x86-commandline/latest/nuget.exe' -OutFile $nuget -UseBasicParsing
    $signature = Get-AuthenticodeSignature $nuget
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
        Remove-Item $nuget
        throw 'Downloaded nuget.exe is not signed by Microsoft.'
    }
}
foreach ($config in 'src\Core', 'src\App', 'tests\CoreTests') {
    & $nuget restore (Join-Path $root "$config\packages.config") -PackagesDirectory (Join-Path $root 'packages') `
        -ConfigFile (Join-Path $root 'nuget.config') -NonInteractive -Verbosity quiet
    if ($LASTEXITCODE -ne 0) { throw "NuGet restore failed for $config\packages.config" }
}

# ---------------------------------------------------------------- analysis
# First, so that the binaries built next (and published) come from a normal
# build rather than an /analyze one.
if ($Analyze) {
    Step 'MSVC code analysis (/analyze, Native Recommended Rules)'
    # The projects compile with /WX, but that leaves some analysis warnings
    # (the C++ Core Check ones, C26xxx) warnings: any warning fails the step.
    Invoke-MSBuild (Join-Path $root 'SpaceWindow.slnx') 'Release' 'x64' @('/t:Rebuild', '/p:RunCodeAnalysis=true',
        '/p:CodeAnalysisRuleSet=NativeRecommendedRules.ruleset') | Tee-Object -Variable lines | ForEach-Object { Write-Host $_ }
    $findings = @($lines | Where-Object { "$_" -match ': warning C\d{4,5}:' } | Sort-Object -Unique)
    if ($findings.Count -gt 0) { throw "Code analysis reported $($findings.Count) finding(s)." }

    Step 'clang-tidy'
    & (Join-Path $root 'tools\Run-ClangTidy.ps1') -Root $root
    if ($LASTEXITCODE -ne 0) { throw 'clang-tidy reported findings.' }

    Step 'clang-format (the style of .clang-format)'
    $format = Join-Path $vs 'VC\Tools\Llvm\x64\bin\clang-format.exe'
    if (-not (Test-Path $format)) { throw 'Visual Studio with the "C++ Clang tools" component is required.' }
    # ChromeThemeKeys.h is generated (tools\make_theme_keys.py).
    $sources = Get-ChildItem (Join-Path $root 'src'), (Join-Path $root 'tests') -Include *.cpp, *.h -Recurse |
        Where-Object { $_.Name -ne 'ChromeThemeKeys.h' }
    & $format --dry-run --Werror --style=file @($sources.FullName)
    if ($LASTEXITCODE -ne 0) { throw 'Some files are not formatted as .clang-format says: run clang-format -i on them.' }
    Write-Host "    $($sources.Count) files as .clang-format says"
}

# ---------------------------------------------------------------- build
Step "Building $Configuration|$Platform"
Invoke-MSBuild (Join-Path $root 'tests\CoreTests\CoreTests.vcxproj') $Configuration $Platform
Invoke-MSBuild (Join-Path $root 'src\App\SpaceWindow.vcxproj') $Configuration $Platform

# ---------------------------------------------------------------- tests
$hostIsArm = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64'
if (($Platform -eq 'ARM64') -eq $hostIsArm) {
    Step 'Running unit tests'
    & (Join-Path $root "bin\$Platform\$Configuration\CoreTests\CoreTests.exe")
    if ($LASTEXITCODE -ne 0) { throw 'Unit tests failed.' }
}
else {
    Write-Host "`n    (skipping unit tests: $Platform binaries cannot run on this machine)" -ForegroundColor Yellow
}

if ($Sanitize) {
    Step 'Unit tests under AddressSanitizer'
    Invoke-MSBuild (Join-Path $root 'tests\CoreTests\CoreTests.vcxproj') 'Asan' 'x64'
    $msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
    $env:PATH = "$(Join-Path $msvc.FullName 'bin\Hostx64\x64');$env:PATH"   # clang_rt.asan_dynamic-x86_64.dll
    $env:ASAN_OPTIONS = 'halt_on_error=1:detect_leaks=0'
    & (Join-Path $root 'bin\x64\Asan\CoreTests\CoreTests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Unit tests failed under AddressSanitizer.' }
}

# ---------------------------------------------------------------- publish
if (-not $NoPublish) {
    Step 'Publishing'
    $output = Join-Path $root "bin\$Platform\$Configuration\SpaceWindow"
    $name = "SpaceWindow-$version-$($Platform.ToLowerInvariant())"
    $target = Join-Path $root "artifacts\$name"
    if (Test-Path $target) { Remove-Item -Recurse -Force $target }
    New-Item -ItemType Directory -Force $target | Out-Null
    # Everything the app needs at run time; debug symbols and link by-products stay behind.
    robocopy $output $target /E /NJH /NJS /NFL /NDL /NP /XF *.pdb *.lib *.exp *.ilk | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
    Copy-Item (Join-Path $root 'README.md'), (Join-Path $root 'LICENSE.txt'), (Join-Path $root 'THIRD-PARTY-NOTICES.md') $target
    Copy-Item (Join-Path $root 'docs') $target -Recurse   # the README's screenshots
    $zip = Join-Path $root "artifacts\$name.zip"
    if (Test-Path $zip) { Remove-Item $zip }
    Compress-Archive -Path "$target\*" -DestinationPath $zip -CompressionLevel Optimal
    $size = [math]::Round((Get-ChildItem $target -Recurse -File | Measure-Object Length -Sum).Sum / 1MB, 1)
    Write-Host "    $target ($size MB)"
    Write-Host "    $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
}

Step 'Done'
