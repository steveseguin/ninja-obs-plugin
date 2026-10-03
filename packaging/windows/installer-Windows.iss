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
AppendDefaultDirName=no
DirExistsWarning=no
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
UninstallDisplayName=OBS VDO.Ninja Plugin
UninstallFilesDir={app}\data\obs-plugins\obs-vdoninja\_installer
ChangesAssociations=no
CloseApplications=yes
CloseApplicationsFilter=obs64.exe,obs32.exe
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
WizardSelectDir=Select OBS Studio
SelectDirDesc=Which OBS Studio installation should receive the plugin?
SelectDirLabel3=Confirm the OBS Studio folder below. For a custom or portable build, choose its folder or use "Select obs64.exe...".
SelectDirBrowseLabel=Choose the existing OBS root containing bin\64bit\obs64.exe, not the bin or obs-plugins subfolder.
ReadyMemoDir=OBS Studio folder:

[Files]
Source: "{#MySourceDir}\obs-plugins\64bit\*"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#MySourceDir}\data\obs-plugins\obs-vdoninja\*"; DestDir: "{app}\data\obs-plugins\obs-vdoninja"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#MySourceDir}\LICENSE"; DestDir: "{app}\data\obs-plugins\obs-vdoninja\docs"; Flags: ignoreversion
Source: "{#MySourceDir}\INSTALL.md"; DestDir: "{app}\data\obs-plugins\obs-vdoninja\docs"; Flags: ignoreversion
Source: "{#MySourceDir}\QUICKSTART.md"; DestDir: "{app}\data\obs-plugins\obs-vdoninja\docs"; Flags: ignoreversion
Source: "{#MySourceDir}\README.md"; DestDir: "{app}\data\obs-plugins\obs-vdoninja\docs"; Flags: ignoreversion
Source: "{#MySourceDir}\THIRD_PARTY_LICENSES.md"; DestDir: "{app}\data\obs-plugins\obs-vdoninja\docs"; Flags: ignoreversion

[Run]
Filename: "{app}\bin\64bit\obs64.exe"; WorkingDir: "{app}\bin\64bit"; Description: "Launch the selected OBS Studio now"; Flags: nowait postinstall skipifsilent unchecked; Check: FileExists(ExpandConstant('{app}\bin\64bit\obs64.exe'))
Filename: "https://steveseguin.github.io/ninja-obs-plugin/#quick-start"; Description: "Open web Quick Start guide"; Flags: shellexec nowait postinstall skipifsilent

[Code]
function IsObsInstallDir(const InstallDir: string): Boolean;
begin
  Result := FileExists(AddBackslash(InstallDir) + 'bin\64bit\obs64.exe') and
    FileExists(AddBackslash(InstallDir) + 'bin\64bit\obs.dll');
end;

function QueryObsInstallDir(const RootKey: Integer; var InstallDir: string): Boolean;
begin
  { Official OBS installers register their root here. }
  Result := RegQueryStringValue(RootKey, 'SOFTWARE\OBS Studio', '', InstallDir);
  if Result and IsObsInstallDir(InstallDir) then
    exit;

  Result := RegQueryStringValue(RootKey,
    'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio_is1',
    'InstallLocation', InstallDir);
  if Result and IsObsInstallDir(InstallDir) then
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

  if QueryObsInstallDir(HKCU64, DetectedDir) then begin
    Result := DetectedDir;
    exit;
  end;

  Result := ExpandConstant('{commonpf64}\obs-studio');
end;

function ObsDirectoryError(const InstallDir: string): string;
begin
  Result := '';
  if not IsObsInstallDir(InstallDir) then
    Result := 'OBS Studio was not found in:' + #13#10 + InstallDir + #13#10#13#10 +
      'Choose the OBS root folder containing bin\64bit\obs64.exe and bin\64bit\obs.dll.' + #13#10 +
      'For a custom or portable build, use "Select obs64.exe..." to locate it.';
end;

procedure SelectObsExecutable(Sender: TObject);
var
  FileName, InstallDir, ErrorMessage: string;
begin
  FileName := '';
  if not GetOpenFileName('Select the OBS Studio executable', FileName,
    WizardDirValue, 'OBS Studio (obs64.exe)|obs64.exe', 'exe') then
    exit;

  InstallDir := ExtractFileDir(ExtractFileDir(ExtractFileDir(FileName)));
  ErrorMessage := ObsDirectoryError(InstallDir);
  if (CompareText(FileName, AddBackslash(InstallDir) + 'bin\64bit\obs64.exe') <> 0) or
    (ErrorMessage <> '') then begin
    MsgBox('Select obs64.exe inside the OBS installation''s bin\64bit folder.', mbError, MB_OK);
    exit;
  end;
  WizardForm.DirEdit.Text := InstallDir;
end;

procedure InitializeWizard;
var
  SelectExeButton: TNewButton;
begin
  SelectExeButton := TNewButton.Create(WizardForm);
  SelectExeButton.Parent := WizardForm.SelectDirPage;
  SelectExeButton.Left := WizardForm.DirEdit.Left;
  SelectExeButton.Top := WizardForm.DirEdit.Top + WizardForm.DirEdit.Height + ScaleY(12);
  SelectExeButton.Width := ScaleX(160);
  SelectExeButton.Height := WizardForm.DirBrowseButton.Height;
  SelectExeButton.Caption := 'Select obs64.exe...';
  SelectExeButton.OnClick := @SelectObsExecutable;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  ErrorMessage: string;
begin
  Result := True;
  if CurPageID = wpSelectDir then begin
    ErrorMessage := ObsDirectoryError(WizardDirValue);
    Result := ErrorMessage = '';
    if not Result then
      SuppressibleMsgBox(ErrorMessage, mbError, MB_OK, IDOK);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): string;
begin
  { Also validate /DIR overrides and silent installs before writing anything. }
  Result := ObsDirectoryError(ExpandConstant('{app}'));
end;
