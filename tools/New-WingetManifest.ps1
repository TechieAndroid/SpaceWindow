<#
.SYNOPSIS
    Writes the winget manifest of a release, for the installers in artifacts\.
.DESCRIPTION
    build.ps1 runs this after building an installer. It writes the three files
    of a winget-pkgs submission to
    artifacts\winget\manifests\r\recompiler\SpaceWindow\<version>\, for the
    installers it finds in artifacts\ (SpaceWindow-<version>-<platform>-setup.exe),
    as downloaded from the repository's GitHub release v<version>.

    The manifest pins each installer's SHA-256: upload the installers from the
    same build, unchanged (README.md, "Publishing a release").
.EXAMPLE
    .\tools\New-WingetManifest.ps1 -Version 1.0.0 -Repository https://github.com/<owner>/SpaceWindow
#>
param(
    [Parameter(Mandatory)] [string] $Version,
    [Parameter(Mandatory)] [string] $Repository,  # https://github.com/<owner>/<repository>
    [string] $Tag = "v$Version",
    [string] $Branch = 'main'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path $PSScriptRoot
$id = 'recompiler.SpaceWindow'
$publisher = 'recompiler'
$schema = '1.12.0'
$Repository = $Repository.TrimEnd('/')

$installers = @(foreach ($arch in 'x64', 'arm64') {
        $file = Join-Path $root "artifacts\SpaceWindow-$Version-$arch-setup.exe"
        if (Test-Path $file) {
            [pscustomobject]@{
                Architecture = $arch
                Url          = "$Repository/releases/download/$Tag/$(Split-Path $file -Leaf)"
                Sha256       = (Get-FileHash $file -Algorithm SHA256).Hash
            }
        }
    })
if ($installers.Count -eq 0) { throw "No installers of SpaceWindow $Version in artifacts\." }

# Windows lists the installed app under the installer's AppId, plus "_is1".
$appId = Select-String -Path (Join-Path $root 'installer\SpaceWindow.iss') -Pattern '^AppId=\{(\{[0-9A-Fa-f-]+\})'
if (-not $appId) { throw 'No AppId in installer\SpaceWindow.iss.' }
$productCode = "$($appId.Matches[0].Groups[1].Value)_is1"
$owner = $Repository -replace '/[^/]+$', ''

$folder = Join-Path $root "artifacts\winget\manifests\$($id.Substring(0, 1).ToLowerInvariant())\$($id -replace '\.', '\')\$Version"
New-Item -ItemType Directory -Force $folder | Out-Null
# Emptied rather than deleted (a terminal may be open in it): wingetcreate
# submit reads every file in it.
Get-ChildItem $folder -Force | Remove-Item -Recurse -Force

function Write-Manifest([string] $name, [string] $type, [string] $body) {
    $text = "# yaml-language-server: `$schema=https://aka.ms/winget-manifest.$type.$schema.schema.json`n`n" +
        "PackageIdentifier: $id`nPackageVersion: $Version`n" + $body.Trim() +
        "`nManifestType: $type`nManifestVersion: $schema`n"
    [IO.File]::WriteAllText((Join-Path $folder $name), ($text -replace "`r`n", "`n"), (New-Object Text.UTF8Encoding $false))
}

Write-Manifest "$id.yaml" 'version' @"
DefaultLocale: en-US
"@

$entries = foreach ($installer in $installers) {
    "- Architecture: $($installer.Architecture)`n  InstallerUrl: $($installer.Url)`n  InstallerSha256: $($installer.Sha256)"
}
Write-Manifest "$id.installer.yaml" 'installer' @"
MinimumOSVersion: 10.0.17763.0
InstallerType: inno
Scope: machine
InstallModes:
- interactive
- silent
- silentWithProgress
UpgradeBehavior: install
ElevationRequirement: elevatesSelf
ProductCode: '$productCode'
AppsAndFeaturesEntries:
- DisplayName: SpaceWindow
  Publisher: $publisher
  ProductCode: '$productCode'
InstallationMetadata:
  DefaultInstallLocation: '%ProgramFiles%\SpaceWindow'
Installers:
$($entries -join "`n")
"@

Write-Manifest "$id.locale.en-US.yaml" 'defaultLocale' @"
PackageLocale: en-US
Publisher: $publisher
PublisherUrl: $owner
PublisherSupportUrl: $Repository/issues
Author: $publisher
PackageName: SpaceWindow
PackageUrl: $Repository
License: MIT
LicenseUrl: $Repository/blob/$Branch/LICENSE.txt
Copyright: Copyright (c) 2026 recompiler; SpaceMonger (c) 1998-2020 Sean Werkema
ShortDescription: See where your disk space goes - a modern Windows 11 remake of the classic SpaceMonger.
Description: >-
  SpaceWindow draws everything on a drive or in a folder as a map: every file is a box sized by the
  space it takes on disk, nested inside the boxes of its folders, so the biggest space hogs are the
  biggest boxes on the screen. The same map can also be drawn as a pie chart, and the whole app can
  wear a theme such as Dracula. Point at anything to see what it is, double-click a folder to zoom
  in, and clean up straight from the map.
Moniker: spacewindow
Tags:
- disk-space
- disk-usage
- pie-chart
- spacemonger
- storage
- treemap
ReleaseNotesUrl: $Repository/releases/tag/$Tag
"@

Write-Host "    $folder"
foreach ($installer in $installers) { Write-Host "      $($installer.Architecture): $($installer.Url)" }
if (Get-Command winget -ErrorAction SilentlyContinue) {
    $result = winget validate --manifest $folder --disable-interactivity
    if ($LASTEXITCODE -ne 0) { throw "winget validate: $($result -join ' ')" }
}
