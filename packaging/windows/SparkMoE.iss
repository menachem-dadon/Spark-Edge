; SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
; Required Notice: Copyright (c) 2026 SparkMoE contributors.

#define AppName "Spark Edge"
#ifndef AppVersion
#define AppVersion "0.5.0-alpha"
#endif
#ifndef SourceDir
#define SourceDir "..\..\artifacts\release\spark-edge-0.5.0-alpha-windows-x64-avx2"
#endif
#ifndef OutputDir
#define OutputDir "..\..\artifacts\release"
#endif
#ifndef Variant
#define Variant "avx2"
#endif

[Setup]
AppId={{CB3A5957-FB37-4D8D-ABB3-F3F61C6432F6}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Spark Edge Project
AppComments=Run and manage local AI models
DefaultDirName={localappdata}\Programs\SparkMoE
DefaultGroupName=Spark Edge
PrivilegesRequired=lowest
UsePreviousGroup=no
UsePreviousTasks=no
OutputDir={#OutputDir}
OutputBaseFilename=spark-edge-{#AppVersion}-windows-x64-{#Variant}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\SparkMoE.exe
SetupIconFile={#SourceDir}\assets\branding\spark-edge.ico
VersionInfoDescription=Spark Edge installer
VersionInfoProductName=Spark Edge
ChangesEnvironment=yes

[Languages]
Name: "hebrew"; MessagesFile: "compiler:Languages\Hebrew.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
#ifdef WebView2Bootstrapper
Source: "{#WebView2Bootstrapper}"; DestName: "MicrosoftEdgeWebview2Setup.exe"; Flags: dontcopy
#endif

[Icons]
Name: "{group}\Spark Edge"; Filename: "{app}\SparkMoE.exe"
Name: "{userdesktop}\Spark Edge"; Filename: "{app}\SparkMoE.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked
Name: "addtopath"; Description: "Add Spark Edge to the user PATH"; GroupDescription: "Command line:"; Flags: unchecked
#ifdef WebView2Bootstrapper
Name: "webview2"; Description: "Install Microsoft WebView2 Runtime for the desktop window (requires Internet)"; GroupDescription: "Optional: Spark Edge can also open in your default browser."; Flags: unchecked; Check: NeedsWebView2
#endif

[Code]
const
  RuntimeKey = 'Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}';
  InstallerKey = 'Software\SparkMoE\Installer';

function HasRuntimeVersion(Root: Integer): Boolean;
var Version: string;
begin
  Result := RegQueryStringValue(Root, RuntimeKey, 'pv', Version) and
    (Version <> '') and (Version <> '0.0.0.0');
end;

function NeedsWebView2: Boolean;
begin
  Result := not (HasRuntimeVersion(HKCU) or HasRuntimeVersion(HKLM32));
end;

function NormalizePathSegment(Value: string): string;
begin
  Value := Trim(Value);
  if (Length(Value) >= 2) and (Value[1] = '"') and (Value[Length(Value)] = '"') then
    Value := Copy(Value, 2, Length(Value) - 2);
  while (Length(Value) > 3) and ((Value[Length(Value)] = '\') or (Value[Length(Value)] = '/')) do
    Delete(Value, Length(Value), 1);
  Result := Lowercase(Value);
end;

function TakePathSegment(var Remaining: string): string;
var Separator: Integer;
begin
  Separator := Pos(';', Remaining);
  if Separator = 0 then begin
    Result := Remaining;
    Remaining := '';
  end else begin
    Result := Copy(Remaining, 1, Separator - 1);
    Delete(Remaining, 1, Separator);
  end;
end;

function ContainsPathSegment(PathValue, AppPath: string): Boolean;
begin
  Result := False;
  AppPath := NormalizePathSegment(AppPath);
  while PathValue <> '' do begin
    if NormalizePathSegment(TakePathSegment(PathValue)) = AppPath then begin
      Result := True;
      Exit;
    end;
  end;
end;

function RemovePathSegment(PathValue, AppPath: string): string;
var Segment: string; First: Boolean;
begin
  Result := '';
  First := True;
  AppPath := NormalizePathSegment(AppPath);
  while PathValue <> '' do begin
    Segment := TakePathSegment(PathValue);
    if NormalizePathSegment(Segment) <> AppPath then begin
      if not First then Result := Result + ';';
      Result := Result + Segment;
      First := False;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var PathValue, AppPath: string;
#ifdef WebView2Bootstrapper
    ResultCode: Integer;
#endif
begin
  if CurStep <> ssPostInstall then Exit;
  if WizardIsTaskSelected('addtopath') then begin
    AppPath := ExpandConstant('{app}');
    RegQueryStringValue(HKCU, 'Environment', 'Path', PathValue);
    if not ContainsPathSegment(PathValue, AppPath) then begin
      if (PathValue <> '') and (PathValue[Length(PathValue)] <> ';') then PathValue := PathValue + ';';
      if RegWriteExpandStringValue(HKCU, 'Environment', 'Path', PathValue + AppPath) then
        RegWriteStringValue(HKCU, InstallerKey, 'AddedPath', AppPath);
    end;
  end;
#ifdef WebView2Bootstrapper
  if WizardIsTaskSelected('webview2') and NeedsWebView2 then begin
    ExtractTemporaryFile('MicrosoftEdgeWebview2Setup.exe');
    if not ExecAsOriginalUser(ExpandConstant('{tmp}\MicrosoftEdgeWebview2Setup.exe'), '/silent /install', '', SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode) or NeedsWebView2 then
      SuppressibleMsgBox('Microsoft WebView2 Runtime could not be installed. Spark Edge will open in your default browser. You can install the Runtime later from Microsoft.', mbInformation, MB_OK, IDOK);
  end;
#endif
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var PathValue, AppPath, AddedPath: string;
begin
  if CurUninstallStep = usUninstall then begin
    AppPath := ExpandConstant('{app}');
    if RegQueryStringValue(HKCU, InstallerKey, 'AddedPath', AddedPath) and
        (NormalizePathSegment(AddedPath) = NormalizePathSegment(AppPath)) then begin
      if RegQueryStringValue(HKCU, 'Environment', 'Path', PathValue) then
        RegWriteExpandStringValue(HKCU, 'Environment', 'Path', RemovePathSegment(PathValue, AppPath));
      RegDeleteValue(HKCU, InstallerKey, 'AddedPath');
    end;
  end;
end;
