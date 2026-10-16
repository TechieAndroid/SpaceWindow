; SpaceWindow's installer, for Inno Setup 6.7 or later (winget install
; JRSoftware.InnoSetup). build.ps1 builds one per platform, from the folder it
; has just published:
;
;   ISCC.exe /DAppVersion=1.0.0 /DArch=x64 /DSourceDir=<that folder>
;            /DOutputDir=<artifacts> [/DAppURL=<repository>] installer\SpaceWindow.iss
;
; It installs for all users, into Program Files: SpaceWindow is often run as
; administrator, and what runs elevated should not sit where any program can
; write. winget runs it silently (tools\New-WingetManifest.ps1).

#ifndef AppVersion
  #error Define AppVersion, e.g. /DAppVersion=1.0.0
#endif
#ifndef SourceDir
  #error Define SourceDir: the folder build.ps1 publishes
#endif
#ifndef OutputDir
  #define OutputDir "..\artifacts"
#endif
#ifndef Arch
  #error Define Arch: x64 or arm64
#elif Arch == "x64"
  ; x64 Windows, or Windows 11 on Arm (which runs x64 programs)
  #define Architectures "x64compatible"
#elif Arch == "arm64"
  #define Architectures "arm64"
#else
  #error Arch must be x64 or arm64
#endif

[Setup]
; Never change AppId: Windows (and winget) know the installed app by it, and a
; new version installs over the old one only if the two share it.
AppId={{8A003DF4-30E5-4985-9D4B-DAE957713083}
AppName=SpaceWindow
AppVersion={#AppVersion}
AppPublisher=recompiler
AppCopyright=Copyright (c) 2026 recompiler
#ifdef AppURL
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/issues
AppUpdatesURL={#AppURL}/releases
#endif
; Settings > Apps lists it as "SpaceWindow", not "SpaceWindow version 1.0.0".
UninstallDisplayName=SpaceWindow
UninstallDisplayIcon={app}\SpaceWindow.exe
DefaultDirName={autopf}\SpaceWindow
; One Start menu entry, no folder around it.
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed={#Architectures}
ArchitecturesInstallIn64BitMode={#Architectures}
; The Windows App SDK's minimum (Windows 10 version 1809).
MinVersion=10.0.17763
WizardStyle=modern dynamic
SetupIconFile=..\src\App\Assets\SpaceWindow.ico
Compression=lzma2/ultra64
SolidCompression=yes
OutputDir={#OutputDir}
OutputBaseFilename=SpaceWindow-{#AppVersion}-{#Arch}-setup
VersionInfoVersion={#AppVersion}
VersionInfoProductName=SpaceWindow
VersionInfoDescription=SpaceWindow Setup

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; The published folder, less what only the download needs: the README and its
; screenshots.
Source: "{#SourceDir}\*"; Excludes: "\README.md,\docs"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\SpaceWindow"; Filename: "{app}\SpaceWindow.exe"
Name: "{autodesktop}\SpaceWindow"; Filename: "{app}\SpaceWindow.exe"; Tasks: desktopicon

[Run]
; Not as administrator: Setup runs elevated, the app it starts should not.
Filename: "{app}\SpaceWindow.exe"; Description: "{cm:LaunchProgram,SpaceWindow}"; Flags: nowait postinstall skipifsilent runasoriginaluser
