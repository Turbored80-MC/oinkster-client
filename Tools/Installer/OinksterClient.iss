; Builds OinksterClient-Setup-<version>.exe from a published launcher folder.
; Run it through Tools\New-Installer.ps1, which publishes the launcher first.
;
;   ISCC.exe /DSourceDir=<publish dir> /DAppVersion=1.0.0 /DVersionLabel="1.0.0 Beta" /DOutputDir=<dir> OinksterClient.iss

#ifndef SourceDir
  #error SourceDir must point at the published launcher folder.
#endif
#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef VersionLabel
  #define VersionLabel AppVersion
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

#define AppName "Oinkster Client"
#define AppExe "OinksterClient.exe"

[Setup]
; Keep AppId fixed across releases so new installers upgrade the old install.
AppId={{6F1C2B7E-3D4A-4E8B-9C21-0A5B7D3E9F64}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#VersionLabel}
AppPublisher=Oinkster
AppPublisherURL=https://oinksterclient.vercel.app
AppSupportURL=https://github.com/Turbored80-MC/oinkster-client/issues
AppUpdatesURL=https://github.com/Turbored80-MC/oinkster-client/releases
VersionInfoVersion={#AppVersion}
; Per-user install: no admin prompt, and the launcher can update in place.
PrivilegesRequired=lowest
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DisableDirPage=yes
DisableReadyPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#OutputDir}
OutputBaseFilename=OinksterClient-Setup-{#AppVersion}
SetupIconFile={#SourceDir}\Assets\OinksterAppIcon.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/ultra64
SolidCompression=yes
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; Clear the previous build's files so nothing stale is left behind on upgrade.
Type: filesandordirs; Name: "{app}\*"

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{userdesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
