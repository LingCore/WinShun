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

; Installed for all users under Program Files: the index service (which
; reads the NTFS master file tables for WinShun, GitHub #21) is registered
; once, by this installer; WinShun itself runs with the user's normal rights.
; The dialog also offers "for me only" (GitHub #52, a company computer
; without administrator rights): under the user's own folders, with no
; service and nothing written for all users; /CURRENTUSER and /ALLUSERS
; choose without the dialog.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
; Off (Inno Setup 6.7 turns it on by default, GitHub #50): the WinShun Setup
; starts in [Run] inherits it, and so does Explorer when WinShun restarts it;
; every program started from the desktop then fails on junctions (Scoop,
; WSL moved to another drive) with "untrusted mount point" until a reboot.
#if Ver >= EncodeVer(6, 7, 0)
RedirectionGuard=no
#endif
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
; The ws task puts {app}\bin on the PATH (the system's or the user's, by the
; install mode): the programs that are open are told to read it again, at the
; end of Setup and of the uninstall alike.
ChangesEnvironment=yes

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

; The install mode dialog (originals: ChineseSimplified.isl / Default.isl): what
; each choice means, in the words of the buttons and above them. The
; "recommended" forms are the same: which one is right depends on the computer.
[Messages]
english.PrivilegesRequiredOverrideText1=%1 can be installed for all users or for you only.%n%nAll users (needs an administrator): with the index service, which indexes whole disks and is the fastest.%nOnly me (no administrator needed): without the service, only the folders you choose are indexed. Choose this on a work computer.
english.PrivilegesRequiredOverrideText2=%1 can be installed for you only or for all users.%n%nAll users (needs an administrator): with the index service, which indexes whole disks and is the fastest.%nOnly me (no administrator needed): without the service, only the folders you choose are indexed. Choose this on a work computer.
english.PrivilegesRequiredOverrideAllUsers=Install for &all users (needs an administrator)
english.PrivilegesRequiredOverrideAllUsersRecommended=Install for &all users (needs an administrator)
english.PrivilegesRequiredOverrideCurrentUser=Install for &me only (no administrator needed)
english.PrivilegesRequiredOverrideCurrentUserRecommended=Install for &me only (no administrator needed)
chinesesimplified.PrivilegesRequiredOverrideText1=%1 可以为所有用户安装，也可以只为您安装。%n%n为所有用户安装（需要管理员）：带索引服务，索引整个磁盘，最快。%n只为我安装（不需要管理员）：不装服务，只索引你选的文件夹。公司电脑选这个。
chinesesimplified.PrivilegesRequiredOverrideText2=%1 可以只为您安装，也可以为所有用户安装。%n%n为所有用户安装（需要管理员）：带索引服务，索引整个磁盘，最快。%n只为我安装（不需要管理员）：不装服务，只索引你选的文件夹。公司电脑选这个。
chinesesimplified.PrivilegesRequiredOverrideAllUsers=为所有用户安装(&A)（需要管理员）
chinesesimplified.PrivilegesRequiredOverrideAllUsersRecommended=为所有用户安装(&A)（需要管理员）
chinesesimplified.PrivilegesRequiredOverrideCurrentUser=只为我安装(&M)（不需要管理员）
chinesesimplified.PrivilegesRequiredOverrideCurrentUserRecommended=只为我安装(&M)（不需要管理员）

[CustomMessages]
english.PersonalInstalled=WinShun is already installed for you only (without administrator rights).%n%nUninstall that copy first (Settings > Apps), then run this Setup again to install for all users.
chinesesimplified.PersonalInstalled=Win顺 已经只为您一个人安装了（没有用管理员权限）。%n%n请先卸载那一份（设置 > 应用），再运行这个安装程序为所有用户安装。
english.AllUsersInstalled=WinShun is already installed for all users on this computer.%n%nJust use it; there is no need to install it again for yourself.
chinesesimplified.AllUsersInstalled=这台电脑已经为所有用户装了 Win顺，直接用它即可，不用再装一份。
english.RemoveData=Also delete WinShun's settings, file index and search history?%n%nKeep them if you are going to install WinShun again.
chinesesimplified.RemoveData=要同时删除 Win顺 的设置、文件索引和搜索记录吗？%n%n如果还会再装 Win顺，可以保留它们。
english.StillRunning=WinShun is still running, probably as administrator, and Setup cannot close it.%n%nRight-click WinShun in the notification area and choose Exit, then click Retry.
chinesesimplified.StillRunning=Win顺 还在运行（可能是以管理员身份运行的），安装程序关不掉它。%n%n请在任务栏右下角右键 Win顺，点“退出”，然后点“重试”。
english.NotClosed=WinShun is still running, so its files cannot be replaced. Exit WinShun and run Setup again.
chinesesimplified.NotClosed=Win顺 还在运行，它的文件换不了。请先退出 Win顺，再运行安装程序。
; The Start menu shortcut users pin to the taskbar (WinShunSearch.exe).
english.SearchShortcut=WinShun Search
chinesesimplified.SearchShortcut=Win顺 搜索
; The command line ws (bin\ws.exe, src/cli): the task that puts its folder on the PATH.
english.WsGroup=Command line:
chinesesimplified.WsGroup=命令行：
english.WsCommand=Add the ws command (search files from the command line)
chinesesimplified.WsCommand=添加 ws 命令（在命令行里搜索文件）
english.WsNewTerminal=To use the ws command, type ws in a newly opened terminal. Terminals that are already open do not see the change (restart VS Code for its terminal).
chinesesimplified.WsNewTerminal=要用 ws 命令，请在新打开的终端里输入 ws。已经开着的终端看不到这个变化（VS Code 的终端要重开 VS Code）。

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
; Checked, as asking costs a click and the command is no use unless it is on the PATH.
; Not a [Registry] entry: the PATH is one value that other programs write
; too, and it is only appended to (AddWsToPath), never replaced.
Name: "wscommand"; Description: "{cm:WsCommand}"; GroupDescription: "{cm:WsGroup}"

[Files]
; Everything build.ps1 -Deploy put in dist\WinShun, the index service
; WinShunService.exe (src/service) among them: it is stopped in
; PrepareToInstall before the files are replaced, and registered again in [Run].
; Installed for the user alone (GitHub #52) there is no service: its file stays out.
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "WinShunService.exe"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\WinShunService.exe"; DestDir: "{app}"; Flags: ignoreversion; Check: IsAdminInstallMode

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
; The taskbar button. Its AppUserModelID, the one WinShunSearch.exe sets for
; itself, lets it ask Windows to be pinned (settings -> 打开 Win顺 -> 固定).
Name: "{autoprograms}\{cm:SearchShortcut}"; Filename: "{app}\WinShunSearch.exe"; AppUserModelID: "LingCore.WinShun.Search"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
; The index service (GitHub #21): registered as the service "WinShun"
; (LocalSystem, automatic start) and started, by the installer's own
; (elevated) rights; installed again, it is pointed at the new files.
Filename: "{app}\WinShunService.exe"; Parameters: "--install"; Flags: runhidden waituntilterminated runascurrentuser; Check: IsAdminInstallMode
; The rest as the user who started Setup, with normal rights (runasoriginaluser):
; WinShun runs so, and its autostart task and settings are that user's, not
; the administrator's who answered UAC for a standard user (GitHub #21).
; An autostart set up earlier (a portable copy, the version before) now
; starts this copy; PrepareToInstall removed this copy's old task, which
; may have been that administrator's (--had-autostart: write one anyway).
Filename: "{app}\{#AppExe}"; Parameters: "--take-autostart {code:TakeAutostartArgs}"; Flags: runhidden waituntilterminated runasoriginaluser
; Started at once, in the tray, while the last page is up: the first start of
; new files is slow (the antivirus reads each one: seconds, tens of them at
; times), and nothing would show for that long after Finish. Finish then has the running
; copy put up its search box, at once (the second start hands it "show"), or,
; unticked, has it quit (CurStepChanged). Not runhidden: a hidden start would
; hide the first window it shows later on.
Filename: "{app}\{#AppExe}"; Parameters: "--background"; Flags: nowait runasoriginaluser
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent runasoriginaluser

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
// First the MCP relays (WinShunMcp.exe): an AI client (Claude, Cursor, VS
// Code) keeps one running for as long as it is open, with Qt6Core.dll and
// msvcp140.dll from the install folder loaded, so those could not be
// replaced (DeleteFile, code 5). Ended before WinShun, too: one would start
// WinShun again through its autostart task when a tool is called.
// False when one is still running: as administrator, from a Setup without
// those rights (GitHub #68), neither --quit nor taskkill can end it, and its
// files (the fonts, the DLLs) could not be replaced. The user is asked to
// quit it then.
function CloseWinShun(Exe: String): Boolean;
var
  ResultCode, Waited: Integer;
begin
  Result := True;
  if Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM WinShunMcp.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
    Sleep(500); // for the files to be let go
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
  while WinShunRunning() do
    if SuppressibleMsgBox(CustomMessage('StillRunning'), mbError, MB_RETRYCANCEL, IDCANCEL) <> IDRETRY then
    begin
      Result := False;
      exit;
    end;
end;

// "Run this program as an administrator" on WinShun.exe (GitHub #68), set
// in its Properties > Compatibility or by the Program Compatibility
// Assistant. WinShun starts with normal rights since 0.8.0 (管理员直读 is
// the way to elevated reads); with it, every start with those rights fails
// (CreateProcess 740: [Run], the autostart task, --quit from a Setup without
// them). Only that word goes; other compatibility settings stay.
procedure RemoveRunAsAdmin(RootKey: Integer);
var
  Key, Exe, Value, Word, Kept: String;
  Space: Integer;
  HasLayer: Boolean;
begin
  Key := 'Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers';
  Exe := ExpandConstant('{app}\{#AppExe}');
  if not RegQueryStringValue(RootKey, Key, Exe, Value) then
    exit;
  Kept := '';
  HasLayer := False;
  Value := Trim(Value) + ' ';
  while Value <> '' do
  begin
    Space := Pos(' ', Value);
    Word := Copy(Value, 1, Space - 1);
    Delete(Value, 1, Space);
    if (Word = '') or (CompareText(Word, 'RUNASADMIN') = 0) then
      continue;
    if Kept <> '' then
      Kept := Kept + ' ';
    Kept := Kept + Word;
    HasLayer := HasLayer or (Length(Word) > 1); // "~", "$": markers, no setting
  end;
  if not HasLayer then
    RegDeleteValue(RootKey, Key, Exe)
  else
    RegWriteStringValue(RootKey, Key, Exe, Kept);
end;

// MoveFileExW: with no new name and MOVEFILE_DELAY_UNTIL_REBOOT, the file
// (or empty folder) is deleted when Windows next starts. NewName 0: NULL.
function MoveFileEx(ExistingName: String; NewName: Cardinal; Flags: Cardinal): Boolean;
  external 'MoveFileExW@kernel32.dll stdcall';

const
  MOVEFILE_DELAY_UNTIL_REBOOT = 4;

// What is left in Folder, files first, then their folders: deleted at the
// next restart.
procedure DeleteAtRestart(Folder: String);
var
  Find: TFindRec;
begin
  if FindFirst(Folder + '\*', Find) then
    try
      repeat
        if (Find.Name <> '.') and (Find.Name <> '..') then
          if (Find.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
            DeleteAtRestart(Folder + '\' + Find.Name)
          else
            MoveFileEx(Folder + '\' + Find.Name, 0, MOVEFILE_DELAY_UNTIL_REBOOT);
      until not FindNext(Find);
    finally
      FindClose(Find);
    end;
  MoveFileEx(Folder, 0, MOVEFILE_DELAY_UNTIL_REBOOT);
end;

// The copies of WinShunTap.dll that Win顺 0.7.1 to 0.11 had Explorer load
// (the search box right of Start, since dropped): {commonappdata}\WinShun\
// tap\<hash>\. An Explorer that loaded one keeps it until it ends, and the
// file cannot be deleted meanwhile: that much goes at the next restart.
// Never a reason for Setup to fail; nothing asks for the restart.
procedure RemoveTapCopies();
var
  Folder: String;
begin
  if not IsAdminInstallMode then
    exit; // installed for the user alone: nothing of ours under ProgramData, and no right to it
  Folder := ExpandConstant('{commonappdata}\WinShun\tap');
  if DirExists(Folder) and not DelTree(Folder, True, True, True) then
    DeleteAtRestart(Folder);
end;

// A portable copy's index service puts its executable in
// %ProgramFiles%\WinShun Service (WinShunService.exe --install, so a SYSTEM
// service cannot be registered from a user-writable folder). Installing for
// all users re-registers the one machine service "WinShun" to
// {app}\WinShunService.exe ([Run] --install), so that portable copy there is
// now stale and unregistered; remove it (admin only — it is admin-writable).
// Not when the service runs from there: installed into a folder ordinary
// users may change (a custom {app} on D:\, say), [Run] --install put its own
// copy there instead. Never under {app}, so uninstalling does not take it;
// done here, not in --uninstall, because the installed service keeps its own
// {app} copy (and --uninstall removes the folder when it ran from it).
procedure RemoveServiceFolder();
var
  Folder, ImagePath: String;
begin
  if not IsAdminInstallMode then
    exit;
  Folder := ExpandConstant('{commonpf}\WinShun Service');
  if not DirExists(Folder) then
    exit;
  if RegQueryStringValue(HKLM, 'SYSTEM\CurrentControlSet\Services\WinShun', 'ImagePath', ImagePath) and
     (Pos(Lowercase(AddBackslash(Folder)), Lowercase(ImagePath)) > 0) then
    exit;
  if not DelTree(Folder, True, True, True) then
    DeleteAtRestart(Folder);
end;

// ---- The command line ws: {app}\bin on the PATH ----------------------------

const
  SystemEnvironmentKey = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment';
  UserEnvironmentKey = 'Environment';
  // Where WinShun's settings page notes the folder it put on the user's PATH.
  CliRecordKey = 'Software\WinShun';
  CliRecordName = 'CliPathEntry';

var
  WsOnPath: Boolean; // the folder is on the PATH now (put there, or there already)

// ws.exe lives alone in {app}\bin, and only that folder goes on the PATH: the
// install folder holds Qt's DLLs, which other programs would find through the
// PATH and use.
function WsFolder(): String;
begin
  Result := ExpandConstant('{app}\bin');
end;

// The PATH of the install mode: the system's for all users, the user's own
// for "only me" (HKA is HKLM or HKCU by the same mode).
function PathKey(): String;
begin
  if IsAdminInstallMode then
    Result := SystemEnvironmentKey
  else
    Result := UserEnvironmentKey;
end;

// Whether a PATH entry names Folder: compared without quotes, a closing
// backslash and case. A %VAR% entry is not expanded: not recognised, it stays.
function EntryIsFolder(Entry, Folder: String): Boolean;
begin
  Entry := Trim(Entry);
  if (Length(Entry) >= 2) and (Entry[1] = '"') and (Entry[Length(Entry)] = '"') then
    Entry := Trim(Copy(Entry, 2, Length(Entry) - 2));
  StringChangeEx(Entry, '/', '\', True);
  while (Length(Entry) > 3) and (Entry[Length(Entry)] = '\') do
    Delete(Entry, Length(Entry), 1);
  Result := CompareText(Entry, Folder) = 0;
end;

function PathHasFolder(Path, Folder: String): Boolean;
var
  Semicolon: Integer;
begin
  Result := False;
  while True do
  begin
    Semicolon := Pos(';', Path);
    if Semicolon = 0 then
    begin
      Result := Result or EntryIsFolder(Path, Folder);
      exit;
    end;
    Result := Result or EntryIsFolder(Copy(Path, 1, Semicolon - 1), Folder);
    Delete(Path, 1, Semicolon);
  end;
end;

// Path without the entries that name Folder, each with one ";"; every other
// character as it was (quotes, %VAR%, empty entries).
function PathWithoutFolder(Path, Folder: String; var Found: Boolean): String;
var
  Semicolon: Integer;
  Part: String;
  First, Last: Boolean;
begin
  Result := '';
  Found := False;
  First := True;
  repeat
    Semicolon := Pos(';', Path);
    Last := Semicolon = 0;
    if Last then
      Part := Path
    else
    begin
      Part := Copy(Path, 1, Semicolon - 1);
      Delete(Path, 1, Semicolon);
    end;
    if EntryIsFolder(Part, Folder) then
      Found := True
    else
    begin
      if not First then
        Result := Result + ';';
      Result := Result + Part;
      First := False;
    end;
  until Last;
end;

// Appended to the PATH of the install mode, as REG_EXPAND_SZ like Windows' own
// (RegQueryStringValue does not expand it, so %VAR% entries go back as they
// were); not twice, which an update would otherwise do. Not when ws.exe is not
// in the folder: no entry for a folder that holds nothing.
procedure AddWsToPath();
var
  Path, Folder: String;
  Present: Boolean;
begin
  Folder := WsFolder();
  if not FileExists(Folder + '\ws.exe') then
    exit;
  Present := RegQueryStringValue(HKA, PathKey(), 'Path', Path);
  if not (Present and PathHasFolder(Path, Folder)) then
  begin
    if (not Present) or (Path = '') then
      Path := Folder
    else if Path[Length(Path)] = ';' then
      Path := Path + Folder
    else
      Path := Path + ';' + Folder;
    RegWriteExpandStringValue(HKA, PathKey(), 'Path', Path);
  end;
  WsOnPath := True;
end;

procedure RemoveFolderFromPath(Root: Integer; Key, Folder: String);
var
  Path: String;
  Found: Boolean;
begin
  if not RegQueryStringValue(Root, Key, 'Path', Path) then
    exit;
  Path := PathWithoutFolder(Path, Folder, Found);
  if Found then
    RegWriteExpandStringValue(Root, Key, 'Path', Path);
end;

// Uninstalling takes that entry out and nothing else: from the PATH of the
// install mode, and, for all users, from the user's own, where the settings
// page may have put it for whoever runs this. The note it keeps of that goes.
procedure RemoveWsFromPath();
var
  Recorded: String;
begin
  RemoveFolderFromPath(HKA, PathKey(), WsFolder());
  if IsAdminInstallMode then
    RemoveFolderFromPath(HKCU, UserEnvironmentKey, WsFolder());
  if RegQueryStringValue(HKCU, CliRecordKey, CliRecordName, Recorded) and EntryIsFolder(Recorded, WsFolder()) then
  begin
    RegDeleteValue(HKCU, CliRecordKey, CliRecordName);
    RegDeleteKeyIfEmpty(HKCU, CliRecordKey);
  end;
end;

// The last page says where to try it: terminals that are open now do not see a new PATH.
procedure CurPageChanged(CurPageID: Integer);
var
  Before: Integer;
begin
  if (CurPageID <> wpFinished) or not WsOnPath then
    exit;
  Before := WizardForm.FinishedLabel.Height;
  WizardForm.FinishedLabel.Caption := WizardForm.FinishedLabel.Caption + #13#10#13#10 + CustomMessage('WsNewTerminal');
  // The label grows with its text (AutoSize); what is under it moves down by as much.
  if WizardForm.FinishedLabel.Height > Before then
  begin
    WizardForm.RunList.Top := WizardForm.RunList.Top + (WizardForm.FinishedLabel.Height - Before);
    WizardForm.RunList.Height := WizardForm.RunList.Height - (WizardForm.FinishedLabel.Height - Before);
  end;
end;

// Started in the tray as the files were in place ([Run]); "run Win顺" left
// unticked on the last page, it goes again. A silent install leaves it
// running, as it was before an update.
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  // The new files in place (an update that failed keeps the old version's copies).
  if CurStep = ssPostInstall then
  begin
    RemoveTapCopies();
    if WizardIsTaskSelected('wscommand') then
      AddWsToPath();
  end;
  // After [Run] re-registered the service from {app}: a portable copy's old
  // service folder is now stale.
  if CurStep = ssDone then
    RemoveServiceFolder();
  if (CurStep = ssDone) and not WizardSilent() and
     ((WizardForm.RunList.Items.Count = 0) or not WizardForm.RunList.Checked[0]) then
    Exec(ExpandConstant('{app}\{#AppExe}'), '--quit', '', SW_HIDE, ewNoWait, ResultCode);
end;

// Stops the index service (the service "WinShun"), so that its file can be
// replaced; waits up to 15 s for it to stop. [Run] starts it again from the
// new files. Nothing to do when it is not installed (a first install).
procedure StopIndexService();
var
  ResultCode, Waited: Integer;
begin
  if not Exec(ExpandConstant('{sys}\sc.exe'), 'stop WinShun', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    exit;
  if ResultCode <> 0 then
    exit; // not installed (1060), or not running (1062)
  Waited := 0;
  while (Waited < 15000) and Exec(ExpandConstant('{cmd}'), '/C sc query WinShun | find "STOPPED"', '', SW_HIDE,
          ewWaitUntilTerminated, ResultCode) and (ResultCode <> 0) do
  begin
    Sleep(500);
    Waited := Waited + 500;
  end;
end;

// Its autostart: the logon task WinShun creates itself, or, where Task
// Scheduler would not take one (GitHub #52), the value "WinShun" in the
// Run key. Only when it starts this copy; a portable copy elsewhere keeps
// its own. True when one went.
function RemoveAutostart(): Boolean;
var
  Listing: AnsiString;
  ListFile, RunCommand: String;
  ResultCode: Integer;
begin
  Result := False;
  if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'WinShun', RunCommand) and
     (Pos(Lowercase(ExpandConstant('{app}\{#AppExe}')), Lowercase(RunCommand)) > 0) then
    Result := RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'WinShun');
  ListFile := AddBackslash(GetTempDir()) + 'WinShun-task.txt';
  if Exec(ExpandConstant('{cmd}'), '/C schtasks /Query /TN WinShun /V /FO LIST > "' + ListFile + '"', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) and LoadStringFromFile(ListFile, Listing) then
    if Pos(Lowercase(ExpandConstant('{app}\{#AppExe}')), Lowercase(String(Listing))) > 0 then
      Result := (Exec(ExpandConstant('{sys}\schtasks.exe'), '/Delete /TN WinShun /F', '', SW_HIDE, ewWaitUntilTerminated, ResultCode)
        and (ResultCode = 0)) or Result;
  DeleteFile(ListFile);
end;

var
  HadAutostart: Boolean;

const
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{C582F95C-EAF2-4AF2-961D-E35C4506652A}_is1';

// Installed for all users and for one user at once would be two copies with
// one set of settings (GitHub #52): the second kind is not started. As an
// administrator: this user has a copy of their own; for the user alone: the
// computer has the all-users one.
function InitializeSetup(): Boolean;
begin
  Result := True;
  if IsAdminInstallMode then
  begin
    if RegKeyExists(HKCU, UninstallKey) then
    begin
      SuppressibleMsgBox(CustomMessage('PersonalInstalled'), mbInformation, MB_OK, IDOK);
      Result := False;
    end;
  end
  else if RegKeyExists(HKLM64, UninstallKey) or RegKeyExists(HKLM32, UninstallKey) then
  begin
    SuppressibleMsgBox(CustomMessage('AllUsersInstalled'), mbInformation, MB_OK, IDOK);
    Result := False;
  end;
end;

// For [Run]: --take-autostart writes the task whether or not one is left
// when PrepareToInstall removed this copy's.
function TakeAutostartArgs(Param: String): String;
begin
  if HadAutostart then
    Result := '--had-autostart'
  else
    Result := '';
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  // First, so that --quit through the installed copy can start (GitHub #68).
  RemoveRunAsAdmin(HKCU);
  if IsAdminInstallMode then
    RemoveRunAsAdmin(HKLM64);
  // The installed copy if there is one; a portable copy is ended.
  if not CloseWinShun(ExpandConstant('{app}\{#AppExe}')) then
  begin
    Result := CustomMessage('NotClosed');
    exit;
  end;
  if IsAdminInstallMode then
    StopIndexService(); // there is no service when installed for the user alone
  // This copy's autostart task of old (GitHub #21): registered elevated by
  // the version before, and by whoever answered UAC then, which for a
  // standard user was an administrator, whose task that user can neither
  // read nor replace. Removed here (elevated); [Run] has the user who
  // started Setup write their own, with normal rights.
  HadAutostart := RemoveAutostart();
  Result := '';
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

// Explorer's right-click "用 Win顺 搜索此处" (src/app/platform/ShellMenu.cpp),
// when it runs this copy's WinShunSearch.exe: a portable copy's stays.
procedure RemoveExplorerMenu();
var
  Keys: array[0..2] of String;
  Command: String;
  I: Integer;
begin
  Keys[0] := 'Software\Classes\Directory\shell\WinShun.SearchHere';
  Keys[1] := 'Software\Classes\Directory\Background\shell\WinShun.SearchHere';
  Keys[2] := 'Software\Classes\Drive\shell\WinShun.SearchHere';
  for I := 0 to 2 do
    if RegQueryStringValue(HKCU, Keys[I] + '\command', '', Command)
      and (Pos(Lowercase(ExpandConstant('{app}\WinShunSearch.exe')), Lowercase(Command)) > 0) then
      RegDeleteKeyIncludingSubkeys(HKCU, Keys[I]);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  // Once the user has confirmed, before the files go.
  if CurUninstallStep = usUninstall then
  begin
    CloseWinShun(ExpandConstant('{app}\{#AppExe}'));
    // The index service: stopped and removed (while its file is still there).
    if IsAdminInstallMode then
      Exec(ExpandConstant('{app}\WinShunService.exe'), '--uninstall', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    RemoveAutostart();
    RemoveExplorerMenu();
    RemoveWsFromPath(); // ChangesEnvironment tells the other programs when the uninstall ends
    // Directory Opus's double click on blank space, if Win顺 set it to run this copy.
    Exec(ExpandConstant('{app}\WinShunSearch.exe'), '--opus-menu-off', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    GiveBackKey('Clipboard', 'WinV', 'V');
    GiveBackKey('Taskbar', 'WinS', 'S');
  end;
  // Old versions' copies of WinShunTap.dll (RemoveTapCopies); the folder
  // itself only when nothing else is in it (the index service's log).
  if CurUninstallStep = usPostUninstall then
  begin
    RemoveTapCopies();
    if IsAdminInstallMode then
      RemoveDir(ExpandConstant('{commonappdata}\WinShun'));
  end;
  if (CurUninstallStep = usPostUninstall) and not UninstallSilent then
    if MsgBox(CustomMessage('RemoveData'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
    begin
      RemoveIndexFolder(IndexFolder()); // before the settings that name it
      DelTree(ExpandConstant('{userappdata}\WinShun'), True, True, True);
      DelTree(ExpandConstant('{localappdata}\WinShun'), True, True, True);
    end;
end;
