#define AppName "QwenStream"
#ifndef AppVersion
#define AppVersion "0.2.0-alpha"
#endif
#ifndef SourceDir
#define SourceDir "..\..\artifacts\release\qwenstream-0.2.0-alpha-windows-x64-avx2"
#endif

[Setup]
AppId={{DA8A90CA-B00F-4E76-A3A0-AB16492F9F79}
AppName={#AppName}
AppVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\QwenStream
DefaultGroupName=QwenStream
PrivilegesRequired=lowest
OutputDir=..\..\artifacts\release
OutputBaseFilename=qwenstream-{#AppVersion}-windows-x64-avx2-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\QwenStream.exe

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\QwenStream"; Filename: "{app}\QwenStream.exe"
Name: "{userdesktop}\QwenStream"; Filename: "{app}\QwenStream.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"
Name: "addtopath"; Description: "Add QwenStream to the user PATH"; GroupDescription: "Command line:"

[Code]
procedure CurStepChanged(CurStep: TSetupStep);
var PathValue: string;
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('addtopath') then begin
    RegQueryStringValue(HKCU, 'Environment', 'Path', PathValue);
    if Pos(ExpandConstant('{app}'), PathValue) = 0 then
      RegWriteExpandStringValue(HKCU, 'Environment', 'Path', PathValue + ';' + ExpandConstant('{app}'));
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var PathValue, AppPath: string;
begin
  if CurUninstallStep = usUninstall then begin
    AppPath := ExpandConstant('{app}');
    if RegQueryStringValue(HKCU, 'Environment', 'Path', PathValue) then begin
      StringChangeEx(PathValue, ';' + AppPath, '', True);
      StringChangeEx(PathValue, AppPath + ';', '', True);
      if CompareText(PathValue, AppPath) = 0 then PathValue := '';
      RegWriteExpandStringValue(HKCU, 'Environment', 'Path', PathValue);
    end;
  end;
end;
