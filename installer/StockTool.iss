; Inno Setup script for StockTool.
; Build:  ISCC.exe /DAppVersion=0.5.1 installer\StockTool.iss
; Output: installer\Output\StockTool-<version>-setup.exe
;
; Installs to Program Files (per-machine) with Start-menu and optional
; desktop shortcuts. The app keeps its config in %APPDATA%\StockTool when
; its own folder is not writable, so nothing user-specific lands here.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#define AppName "StockTool"
#define AppExe  "StockTool.exe"
#define RepoRoot ".."

[Setup]
AppId={{9B9E3C0E-3B7A-4F5B-9F0A-5C1E1D4E7A21}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=B. Graham
AppPublisherURL=https://github.com/Flinterpop/StockTool
AppSupportURL=https://github.com/Flinterpop/StockTool/issues
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\{#AppExe}
OutputDir=Output
OutputBaseFilename={#AppName}-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
LicenseFile=
SetupIconFile={#RepoRoot}\src\StockTool.ico
VersionInfoVersion={#AppVersion}
CloseApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "startup"; Description: "Start {#AppName} when I sign in"; GroupDescription: "Startup:"; Flags: unchecked

[Files]
; Only build output and documentation are packaged. No config file: the app
; writes a default one on first run.
Source: "{#RepoRoot}\build\Release\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoRoot}\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon
Name: "{userstartup}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: startup

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
