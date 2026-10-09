; Win顺 installer (Inno Setup 6.5 or later: the Windows 11 wizard style with
; dark mode). Built by scripts/release.ps1, which passes:
;   /DAppVersion=0.2.1  /DSourceDir=<dist\WinShun>  /DOutputDir=<dist>
; Upgrading is installing again: the same AppId replaces the old version in
; place, and the user's settings and index (in %APPDATA% / %LOCALAPPDATA%)
; are not touched.

#ifndef AppVersion
  #error Pass /DAppVersion=x.y.z
#endif
#ifndef SourceDir
  #define SourceDir "..\dist\WinShun"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

#define AppName "Win顺"
#define AppExe "WinShun.exe"
#define Repository "https://github.com/LingCore/WinShun"

[Setup]
; Never change the AppId: it is how a new version finds the old one.
AppId={{C582F95C-EAF2-4AF2-961D-E35C4506652A}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=LingCore
AppPublisherURL={#Repository}
AppSupportURL={#Repository}/issues
AppUpdatesURL={#Repository}/releases
AppCopyright=Copyright © 2026 LingCore
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName} · WinShun
VersionInfoDescription={#AppName} Setup
VersionInfoCompany=LingCore

; WinShun runs as administrator (it reads the NTFS master file table), so it
; is installed for all users under Program Files.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
DefaultDirName={autopf}\WinShun
DisableProgramGroupPage=yes
DisableWelcomePage=yes
DisableReadyPage=yes
UsePreviousAppDir=yes
UsePreviousTasks=yes
; It is closed in PrepareToInstall, the way it saves its index.
CloseApplications=no
RestartApplications=no

WizardStyle=modern dynamic windows11
; Win顺's own pictures, light and dark (tools/make_installer_images.py).
WizardImageFile=wizard-light.png
WizardImageFileDynamicDark=wizard-dark.png
WizardSmallImageFile=wizard-small.png
WizardSmallImageFileDynamicDark=wizard-small.png
SetupIconFile=..\resources\app.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}

ShowLanguageDialog=no
LanguageDetectionMethod=uilanguage

OutputDir={#OutputDir}
OutputBaseFilename=WinShun-{#AppVersion}-x64-setup
Compression=lzma2/ultra64
SolidCompression=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "ChineseSimplified.isl"

[CustomMessages]
english.RemoveData=Also delete WinShun's settings, file index and search history?%n%nKeep them if you are going to install WinShun again.
chinesesimplified.RemoveData=要同时删除 Win顺 的设置、文件索引和搜索记录吗？%n%n如果还会再装 Win顺，可以保留它们。
; The Start menu shortcut users pin to the taskbar (WinShunSearch.exe).
english.SearchShortcut=WinShun Search
chinesesimplified.SearchShortcut=Win顺 搜索

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
; Programs cannot pin themselves: the user pins this one (settings → 打开 Win顺 → 找到按钮).
Name: "{autoprograms}\{cm:SearchShortcut}"; Filename: "{app}\WinShunSearch.exe"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
; As the installer runs (elevated), so there is no UAC prompt.
; An autostart set up earlier (a portable copy) now starts this copy.
Filename: "{app}\{#AppExe}"; Parameters: "--take-autostart"; Flags: runhidden waituntilterminated runascurrentuser
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent runascurrentuser

[Code]
const
  MessageWindowClass = 'WinShun.MessageWindow';
  WM_CLOSE = $0010;

function WinShunRunning(): Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(ExpandConstant('{cmd}'), '/C tasklist /NH /FI "IMAGENAME eq {#AppExe}" | find /I "{#AppExe}"',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

// Asks a running WinShun to quit, the way it saves its index first: with
// --quit through the installed copy, or WM_CLOSE to its message window (a
// portable copy elsewhere; WinShun 0.2.1 and later). One still running after
// a while is ended.
procedure CloseWinShun(Exe: String);
var
  ResultCode, Waited: Integer;
begin
  if not WinShunRunning() then
    exit;
  if FileExists(Exe) then
    Exec(Exe, '--quit', '', SW_HIDE, ewWaitUntilTerminated, ResultCode)
  else
    PostMessage(FindWindowByClassName(MessageWindowClass), WM_CLOSE, 0, 0);
  Waited := 0;
  while WinShunRunning() and (Waited < 15000) do
  begin
    Sleep(500);
    Waited := Waited + 500;
  end;
  if WinShunRunning() then
  begin
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#AppExe}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(1000);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  // The installed copy if there is one; a portable copy is ended.
  CloseWinShun(ExpandConstant('{app}\{#AppExe}'));
  Result := '';
end;

// Its autostart: the logon task WinShun creates itself. Only when it starts
// this copy; a portable copy elsewhere keeps its own.
procedure RemoveAutostart();
var
  Listing: AnsiString;
  ListFile: String;
  ResultCode: Integer;
begin
  ListFile := AddBackslash(GetTempDir()) + 'WinShun-task.txt';
  if Exec(ExpandConstant('{cmd}'), '/C schtasks /Query /TN WinShun /V /FO LIST > "' + ListFile + '"', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) and LoadStringFromFile(ListFile, Listing) then
    if Pos(Lowercase(ExpandConstant('{app}\{#AppExe}')), Lowercase(String(Listing))) > 0 then
      Exec(ExpandConstant('{sys}\schtasks.exe'), '/Delete /TN WinShun /F', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  DeleteFile(ListFile);
end;

// Win+V back to Windows' clipboard history, and Win+S (with Win+Shift+S) to
// its search, if WinShun had taken them over: the letter it put into
// Explorer's DisabledHotkeys goes (Explorer reads that when it starts). A
// letter there without WinShun having taken the key belongs to someone else.
procedure GiveBackKey(Section, Name, Key: String);
var
  Keys: String;
  I: Integer;
begin
  if CompareText(GetIniString(Section, Name, 'false', ExpandConstant('{userappdata}\WinShun\WinShun.ini')), 'true') <> 0 then
    exit;
  if not RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced', 'DisabledHotkeys', Keys) then
    exit;
  for I := Length(Keys) downto 1 do
    if Uppercase(Keys[I]) = Key then
      Delete(Keys, I, 1);
  if Keys = '' then
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced', 'DisabledHotkeys')
  else
    RegWriteStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced', 'DisabledHotkeys', Keys);
end;

// The folder the index was moved to in the settings ([Index] Folder), or ''
// for the default one. Read by hand: GetIniString would read the file in the
// ANSI code page, and the path may well be Chinese.
function IndexFolder(): String;
var
  Lines: TArrayOfString;
  I, Eq: Integer;
  InIndex: Boolean;
  Line: String;
begin
  Result := '';
  if not LoadStringsFromFile(ExpandConstant('{userappdata}\WinShun\WinShun.ini'), Lines) then
    exit;
  InIndex := False;
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    Line := Trim(Lines[I]);
    if (Length(Line) > 0) and (Line[1] = '[') then
      InIndex := CompareText(Line, '[Index]') = 0
    else if InIndex then
    begin
      Eq := Pos('=', Line);
      if (Eq > 0) and (CompareText(Trim(Copy(Line, 1, Eq - 1)), 'Folder') = 0) then
      begin
        Result := Trim(Copy(Line, Eq + 1, Length(Line)));
        if (Length(Result) >= 2) and (Result[1] = '"') and (Result[Length(Result)] = '"') then
          Result := Copy(Result, 2, Length(Result) - 2);
        StringChangeEx(Result, '/', '\', True);
        exit;
      end;
    end;
  end;
end;

// Only Win顺's own files there go (the folder itself once it is empty):
// it may be a folder the user picked.
procedure RemoveIndexFolder(Folder: String);
begin
  if (Folder = '') or not DirExists(Folder) then
    exit;
  DeleteFile(Folder + '\index.bin');
  DelTree(Folder + '\content\*.grams', False, True, False);
  DelTree(Folder + '\content\*.texts', False, True, False);
  RemoveDir(Folder + '\content');
  RemoveDir(Folder);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  // Once the user has confirmed, before the files go.
  if CurUninstallStep = usUninstall then
  begin
    CloseWinShun(ExpandConstant('{app}\{#AppExe}'));
    RemoveAutostart();
    GiveBackKey('Clipboard', 'WinV', 'V');
    GiveBackKey('Taskbar', 'WinS', 'S');
  end;
  if (CurUninstallStep = usPostUninstall) and not UninstallSilent then
    if MsgBox(CustomMessage('RemoveData'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
    begin
      RemoveIndexFolder(IndexFolder()); // before the settings that name it
      DelTree(ExpandConstant('{userappdata}\WinShun'), True, True, True);
      DelTree(ExpandConstant('{localappdata}\WinShun'), True, True, True);
    end;
end;
