; Runs the install mode decision of WinShun.iss (InstallMode.iss) and installs
; nothing: a Setup with its own AppId (and so its own uninstall entries), no
; files, no service. Each process appends what it decided to /PROBELOG=<file>
; and ends; one started for all users (/ALLUSERS, elevated) does too.
;   ISCC /DOutputDir=<folder> installer\tests\InstallModeProbe.iss
; Parameters (besides Setup's own):
;   /PROBELOG=<file>      where the lines go (required)
;   /PROBEANSWER=first|second|cancel   answers the dialog; without it, it is shown
;   /PROBEREFUSE=<code>   the run for all users "gets no administrator rights":
;                         RelaunchForAllUsers does not start it and returns <code>
;   /PROBEEXIT=<code>     the process that would install ends with <code>
; Fake previous installs: HKLM / HKCU \Software\Microsoft\Windows\CurrentVersion\
; Uninstall\{6E1D3F4A-5B2C-4D8E-9F70-1A2B3C4D5E6F}_is1 (create, test, delete).

#ifndef OutputDir
  #define OutputDir "."
#endif
#define InstallModeProbe

[Setup]
AppId={{6E1D3F4A-5B2C-4D8E-9F70-1A2B3C4D5E6F}
AppName=Win顺
AppVersion=0.0.0
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline
; As in WinShun.iss.
#if Ver >= EncodeVer(6, 7, 0)
RedirectionGuard=no
#endif
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DefaultDirName={autopf}\WinShunInstallModeProbe
CreateAppDir=no
Uninstallable=no
ShowLanguageDialog=no
LanguageDetectionMethod=uilanguage
WizardStyle=modern dynamic windows11
OutputDir={#OutputDir}
OutputBaseFilename=InstallModeProbe

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "..\ChineseSimplified.isl"

[Code]
function GetCurrentProcessId(): DWORD;
  external 'GetCurrentProcessId@kernel32.dll stdcall';

procedure ProbeWrite(Text: String);
begin
  SaveStringsToUTF8FileWithoutBOM(ExpandConstant('{param:PROBELOG|}'), [Text], True);
end;

procedure ProbeNote(Line: String);
begin
  ProbeWrite(Format('  [%d] %s', [GetCurrentProcessId(), Line]));
end;

function ProbeAnswer(): Integer;
var
  Answer: String;
begin
  Answer := Lowercase(ExpandConstant('{param:PROBEANSWER|}'));
  if Answer = 'first' then
    Result := IDYES
  else if Answer = 'second' then
    Result := IDNO
  else if Answer = 'cancel' then
    Result := IDCANCEL
  else
    Result := 0;
end;

function ProbeRefuseRelaunch(): Integer;
begin
  Result := StrToIntDef(ExpandConstant('{param:PROBEREFUSE|0}'), 0);
end;

#include "..\InstallMode.iss"

[Code]
function InitializeSetup(): Boolean;
var
  I: Integer;
  Args: String;
begin
  Args := '';
  for I := 1 to ParamCount do
    Args := Args + '[' + ParamStr(I) + ']';
  ProbeWrite(Format('--- pid %d, admin = %d, args %s', [GetCurrentProcessId(), Ord(IsAdmin()), Args]));
  Result := InstallModeChosen('Software\Microsoft\Windows\CurrentVersion\Uninstall\{6E1D3F4A-5B2C-4D8E-9F70-1A2B3C4D5E6F}_is1');
  ProbeWrite(Format('  [%d] result = %d, admin install mode = %d, language = %s', [GetCurrentProcessId(), Ord(Result), Ord(IsAdminInstallMode), ActiveLanguage]));
  if ExpandConstant('{param:PROBEEXIT|}') <> '' then
    InstallModeExitProcess(StrToIntDef(ExpandConstant('{param:PROBEEXIT|}'), 0));
  Result := False; // never installs
end;
