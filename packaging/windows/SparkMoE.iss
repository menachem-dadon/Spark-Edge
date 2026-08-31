; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
; Required Notice: Copyright (c) 2026 SparkMoE contributors.

#define AppName "SparkMoE"
#ifndef AppVersion
#define AppVersion "0.4.0-alpha"
#endif
#ifndef SourceDir
#define SourceDir "..\..\artifacts\release\sparkmoe-0.4.0-alpha-windows-x64-avx2"
#endif
#ifndef OutputDir
#define OutputDir "..\..\artifacts\release"
#endif

[Setup]
AppId={{CB3A5957-FB37-4D8D-ABB3-F3F61C6432F6}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=SparkMoE Project
AppComments=Run giant local language models on modest hardware
DefaultDirName={localappdata}\Programs\SparkMoE
DefaultGroupName=SparkMoE
PrivilegesRequired=lowest
UsePreviousGroup=no
UsePreviousTasks=no
OutputDir={#OutputDir}
OutputBaseFilename=sparkmoe-{#AppVersion}-windows-x64-avx2-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\SparkMoE.exe
SetupIconFile={#SourceDir}\assets\branding\SparkMoE.ico
VersionInfoDescription=SparkMoE installer
VersionInfoProductName=SparkMoE

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\SparkMoE"; Filename: "{app}\SparkMoE.exe"
Name: "{userdesktop}\SparkMoE"; Filename: "{app}\SparkMoE.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked
Name: "addtopath"; Description: "Add SparkMoE to the user PATH"; GroupDescription: "Command line:"; Flags: unchecked

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
