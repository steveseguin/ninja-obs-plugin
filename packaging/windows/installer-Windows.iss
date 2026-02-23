#ifndef MyAppVersion
  #define MyAppVersion "0.0.0-dev"
#endif

#ifndef MySourceDir
  #define MySourceDir "install"
#endif

#ifndef MyOutputDir
  #define MyOutputDir "."
#endif

#ifndef MyOutputBaseFilename
  #define MyOutputBaseFilename "obs-vdoninja-windows-x64-setup"
#endif

[Setup]
AppId={{A95D1933-7F52-44D5-89B2-67FE58DC4C52}
AppName=OBS VDO.Ninja Plugin
AppVersion={#MyAppVersion}
AppPublisher=VDO.Ninja Community
AppPublisherURL=https://vdo.ninja
AppSupportURL=https://github.com/steveseguin/ninja-obs-plugin/issues
AppUpdatesURL=https://github.com/steveseguin/ninja-obs-plugin/releases
DefaultDirName={code:GetDefaultObsInstallDir}
DisableDirPage=no
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
OutputDir={#MyOutputDir}
OutputBaseFilename={#MyOutputBaseFilename}
LicenseFile={#MySourceDir}\LICENSE
UninstallDisplayName=OBS VDO.Ninja Plugin
ChangesAssociations=no
CloseApplications=yes
CloseApplicationsFilter=obs64.exe,obs32.exe
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#MySourceDir}\obs-plugins\64bit\*"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#MySourceDir}\data\obs-plugins\obs-vdoninja\*"; DestDir: "{app}\data\obs-plugins\obs-vdoninja"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#MySourceDir}\LICENSE"; DestDir: "{app}\data\obs-plugins\obs-vdoninja\docs"; Flags: ignoreversion
Source: "{#MySourceDir}\INSTALL.md"; DestDir: "{app}\data\obs-plugins\obs-vdoninja\docs"; Flags: ignoreversion
Source: "{#MySourceDir}\QUICKSTART.md"; DestDir: "{app}\data\obs-plugins\obs-vdoninja\docs"; Flags: ignoreversion
Source: "{#MySourceDir}\README.md"; DestDir: "{app}\data\obs-plugins\obs-vdoninja\docs"; Flags: ignoreversion
Source: "{#MySourceDir}\THIRD_PARTY_LICENSES.md"; DestDir: "{app}\data\obs-plugins\obs-vdoninja\docs"; Flags: ignoreversion

[Run]
Filename: "{app}\bin\64bit\obs64.exe"; Description: "Launch OBS Studio now"; Flags: nowait postinstall skipifsilent unchecked; Check: FileExists(ExpandConstant('{app}\bin\64bit\obs64.exe'))
Filename: "notepad.exe"; Parameters: """{app}\data\obs-plugins\obs-vdoninja\docs\QUICKSTART.md"""; Description: "Open Quick Start guide"; Flags: nowait postinstall skipifsilent; Check: FileExists(ExpandConstant('{app}\data\obs-plugins\obs-vdoninja\docs\QUICKSTART.md'))

[Code]
function QueryObsInstallDir(const RootKey: Integer; var InstallDir: string): Boolean;
begin
  Result := RegQueryStringValue(RootKey,
    'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio_is1',
    'InstallLocation', InstallDir);
  if Result and (InstallDir <> '') and DirExists(InstallDir) then
    exit;

  Result := False;
  InstallDir := '';
end;

function GetDefaultObsInstallDir(Param: string): string;
var
  DetectedDir: string;
begin
  if QueryObsInstallDir(HKLM64, DetectedDir) then begin
    Result := DetectedDir;
    exit;
  end;

  if QueryObsInstallDir(HKCU, DetectedDir) then begin
    Result := DetectedDir;
    exit;
  end;

  Result := ExpandConstant('{autopf64}\obs-studio');
end;
