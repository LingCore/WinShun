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
; An AI tool using WinShun through its MCP relay (GitHub #97), asked about
; before anything is closed; %1 names the tools (Claude Code, Cursor …).
english.AiInUseTitle=An AI tool is using WinShun
chinesesimplified.AiInUseTitle=有 AI 工具正在用 Win顺
english.AiInUse=Using WinShun right now: %1.%n%nInstalling now disconnects it from WinShun. When Setup is done, reconnect WinShun in it, or restart it.
chinesesimplified.AiInUse=%1 正在用 Win顺。%n%n现在安装，它会和 Win顺 断开。%n装好后在它里面重新连接 Win顺，或者重启它就行。
english.AiInstallNow=&Install now
chinesesimplified.AiInstallNow=现在安装(&I)
english.AiInstallNowNote=The AI tool is disconnected until you reconnect it
chinesesimplified.AiInstallNowNote=AI 工具会断开，装好后重新连接
english.AiInstallLater=&Later
chinesesimplified.AiInstallLater=以后再装(&L)
english.AiInstallLaterNote=Nothing is changed; run Setup again when it is done
chinesesimplified.AiInstallLaterNote=什么都不动，等它用完再运行安装程序
english.AiLater=Nothing was changed. Run Setup again when the AI tool is done with WinShun.
chinesesimplified.AiLater=什么都没改。等 AI 工具用完 Win顺，再运行安装程序。
english.AiTool=An AI tool
chinesesimplified.AiTool=AI 工具
; Still holding WinShun's files after it was closed (Restart Manager names
; them): %1 is the list, one per line.
english.FilesInUseTitle=Some programs are still using WinShun's files
chinesesimplified.FilesInUseTitle=还有程序在用 Win顺 的文件
english.FilesInUse=While these are open, Setup cannot replace WinShun's files:%n%n%1%nClose them, then click Try again. One that runs as administrator has to be closed where it runs.
chinesesimplified.FilesInUse=这些程序开着，Win顺 的文件就换不了：%n%n%1%n请先关掉它们，再点“重试”。%n以管理员身份运行的，要在它那边关。
english.FilesInUseAi=%1 (connected to WinShun)
chinesesimplified.FilesInUseAi=%1（连着 Win顺）
english.FilesNotFreed=Other programs are still using WinShun's files, so nothing was installed. Close them and run Setup again.
chinesesimplified.FilesNotFreed=还有程序在用 Win顺 的文件，这次没有安装。关掉它们后再运行安装程序。
; The Start menu shortcut users pin to the taskbar (WinShunSearch.exe).
english.SearchShortcut=WinShun Search
chinesesimplified.SearchShortcut=Win顺 搜索

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Everything build.ps1 -Deploy put in dist\WinShun, the index service
; WinShunService.exe (src/service) among them: it is stopped in
; PrepareToInstall before the files are replaced, and registered again in [Run].
; Installed for the user alone (GitHub #52) there is no service: its file stays out.
; The MCP relay WinShunMcp.exe comes last (GitHub #97). PrepareToInstall moved
; the old one aside, so an AI client cannot start it again until every other
; file is new: one started earlier would load Qt6Core.dll and the C++ runtime
; from here again, and with --start-winshun start Win顺 through its autostart
; task while the qml\ and plugin folders are still being replaced.
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "WinShunService.exe,\WinShunMcp.exe"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\WinShunService.exe"; DestDir: "{app}"; Flags: ignoreversion; Check: IsAdminInstallMode
Source: "{#SourceDir}\WinShunMcp.exe"; DestDir: "{app}"; Flags: ignoreversion

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
; LAN transfer (the bundled plugin winshun.localsend, GitHub #113): phones and
; computers on the same network reach it on its LocalSend ports, from the
; local subnet only, on any network type (the plugin itself asks before it
; receives on a public one). Win顺 never changes the firewall; this admin
; install does, anew every time (silent ones too); the uninstaller removes it.
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""WinShun LAN transfer"""; Flags: runhidden waituntilterminated runascurrentuser; Check: IsAdminInstallMode
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""WinShun LAN transfer"" dir=in action=allow program=""{app}\plugins\winshun.localsend\WinShunLocalSend.exe"" protocol=TCP localport=53317-53320 remoteip=localsubnet profile=any enable=yes"; Flags: runhidden waituntilterminated runascurrentuser; Check: IsAdminInstallMode
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""WinShun LAN transfer"" dir=in action=allow program=""{app}\plugins\winshun.localsend\WinShunLocalSend.exe"" protocol=UDP localport=53317 remoteip=localsubnet profile=any enable=yes"; Flags: runhidden waituntilterminated runascurrentuser; Check: IsAdminInstallMode
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

[UninstallRun]
; LAN transfer's firewall rules ([Run]): they go with Win顺.
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""WinShun LAN transfer"""; Flags: runhidden waituntilterminated; Check: IsAdminInstallMode; RunOnceId: "DelLanTransferRules"

[UninstallDelete]
; The relay MoveRelayAside put aside when a Setup ended before the new one was
; in (power cut), or that an AI tool still ran as administrator (GitHub #97):
; without this {app} would stay behind with that one file.
Type: files; Name: "{app}\WinShunMcp.exe.old"

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

// ---- AI tools using Win顺 (GitHub #97) ----------------------------------------
// They run its MCP relay, WinShunMcp.exe, with Qt6Core.dll and the C++
// runtime from {app} loaded; the program that started a relay is the tool.

var
  Wmi: Variant; // WMI's root\cimv2, once connected
  WmiAsked: Boolean;

function WmiReady(): Boolean;
var
  Locator: Variant;
begin
  if not WmiAsked then
  begin
    WmiAsked := True;
    try
      Locator := CreateOleObject('WbemScripting.SWbemLocator');
      Wmi := Locator.ConnectServer('.', 'root\cimv2');
    except
      Log('WMI: ' + GetExceptionMessage);
    end;
  end;
  Result := not VarIsEmpty(Wmi);
end;

function VarText(const Value: Variant): String;
begin
  if VarIsNull(Value) or VarIsEmpty(Value) then
    Result := ''
  else
    Result := Value;
end;

// Win32_Process of one process; False when it is gone (or WMI is not there).
function ProcessInfo(Pid: Integer; var Name, Path, Command: String; var Parent: Integer): Boolean;
var
  Found, Item: Variant;
begin
  Result := False;
  if not WmiReady() then
    exit;
  try
    Found := Wmi.ExecQuery(Format('SELECT Name, ExecutablePath, CommandLine, ParentProcessId FROM Win32_Process WHERE ProcessId = %d', [Pid]));
    if Found.Count = 0 then
      exit;
    Item := Found.ItemIndex(0);
    Name := VarText(Item.Name);
    Path := Lowercase(VarText(Item.ExecutablePath));
    Command := Lowercase(VarText(Item.CommandLine));
    Parent := Item.ParentProcessId;
    Result := True;
  except
    Log('WMI: ' + GetExceptionMessage);
  end;
end;

// The AI tool that started a relay, by the name its users know: the first
// program up from it that is not a shell, runtime or terminal in between
// (cmd.exe, node.exe running npx …; an elevated one's command line cannot be
// read). '' when none is found.
function AiToolOf(Relay: Integer): String;
var
  Name, Path, Command, Image: String;
  Pid, Parent, Depth: Integer;
begin
  Result := '';
  if not ProcessInfo(Relay, Name, Path, Command, Pid) then
    exit;
  for Depth := 1 to 6 do
  begin
    if (Pid = 0) or not ProcessInfo(Pid, Name, Path, Command, Parent) then
      exit;
    Image := Lowercase(Name);
    if Image = 'claude.exe' then
    begin
      // Claude Desktop lives in AnthropicClaude (or the Store's WindowsApps); the claude command anywhere else,
      // and one whose path cannot be read: run as administrator, which is Claude Code in such a terminal.
      if (Pos('\anthropicclaude\', Path) > 0) or (Pos('\windowsapps\', Path) > 0) then
        Result := 'Claude'
      else
        Result := 'Claude Code';
    end
    else if (Image = 'node.exe') and (Pos('claude-code', Command) > 0) then
      Result := 'Claude Code'
    else if (Image = 'node.exe') and (Pos('gemini', Command) > 0) then
      Result := 'Gemini CLI'
    else if (Image = 'code.exe') or (Image = 'code - insiders.exe') then
      Result := 'VS Code'
    else if Image = 'cursor.exe' then
      Result := 'Cursor'
    else if Image = 'windsurf.exe' then
      Result := 'Windsurf'
    else if (Image = 'explorer.exe') or (Image = 'svchost.exe') or (Image = 'services.exe') then
      exit
    else if (Image <> 'cmd.exe') and (Image <> 'conhost.exe') and (Image <> 'powershell.exe') and
            (Image <> 'pwsh.exe') and (Image <> 'node.exe') and (Image <> 'uv.exe') and (Image <> 'uvx.exe') and
            (Image <> 'bun.exe') and (Image <> 'deno.exe') and (Image <> 'windowsterminal.exe') and
            (Image <> 'openconsole.exe') then
      Result := ChangeFileExt(Name, ''); // another tool, by its program's name
    if Result <> '' then
      exit;
    Pid := Parent;
  end;
end;

// The ids of the processes running Image from this {app} (GitHub #97): not
// those of a portable copy elsewhere, or of another user's install, which a
// per-user Setup has no business asking about or ending, nor an all-users
// Setup ending in every session. Found by name, then compared by path (no
// WQL escaping of a path with an apostrophe in it); a process whose path
// cannot be read (another user's, elevated) is not this install's. A running
// file renamed by MoveRelayAside is reported under its new name (.old).
// Without WMI nothing is found: the files' holders are named by FilesInUse
// (Restart Manager) all the same, and the user asked to close them.
function OurProcesses(Image: String): TStringList;
var
  Found, Item: Variant;
  Own, Path: String;
  I: Integer;
begin
  Result := TStringList.Create;
  if not WmiReady() then
    exit;
  Own := Lowercase(AddBackslash(ExpandConstant('{app}')) + Image);
  try
    Found := Wmi.ExecQuery('SELECT ProcessId, ExecutablePath FROM Win32_Process WHERE Name = ''' + Image + '''');
    for I := 0 to Found.Count - 1 do
    begin
      Item := Found.ItemIndex(I);
      Path := Lowercase(VarText(Item.ExecutablePath));
      if (Path = Own) or (Path = Own + '.old') then
        Result.Add(IntToStr(Item.ProcessId));
    end;
  except
    Log('WMI: ' + GetExceptionMessage);
  end;
end;

// Ends this {app}'s processes of Image; True when there were some.
function EndOurProcesses(Image: String): Boolean;
var
  Pids: TStringList;
  I, ResultCode: Integer;
begin
  Pids := OurProcesses(Image);
  try
    Result := Pids.Count > 0;
    for I := 0 to Pids.Count - 1 do
      Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /PID ' + Pids[I], '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  finally
    Pids.Free;
  end;
end;

// The AI tools using Win顺 now, each named once ("Claude Code、Cursor"); ''
// when no relay of this install runs.
function AiTools(): String;
var
  Pids, Names: TStringList;
  I: Integer;
  Name: String;
begin
  Result := '';
  Names := TStringList.Create;
  Pids := OurProcesses('WinShunMcp.exe');
  try
    for I := 0 to Pids.Count - 1 do
    begin
      Name := AiToolOf(StrToInt(Pids[I]));
      Log(Format('MCP relay %s, started by %s', [Pids[I], Name]));
      if Name = '' then
        Name := CustomMessage('AiTool');
      if Names.IndexOf(Name) < 0 then
        Names.Add(Name);
    end;
    for I := 0 to Names.Count - 1 do
    begin
      if (I > 0) and (ActiveLanguage = 'chinesesimplified') then
        Result := Result + '、'
      else if I > 0 then
        Result := Result + ', ';
      Result := Result + Names[I];
    end;
  finally
    Names.Free;
    Pids.Free;
  end;
end;

var
  RelayMoved: String; // where MoveRelayAside put {app}\WinShunMcp.exe, '' when it did not

// The relay's file goes out of the way before the relays are ended (Windows
// lets a running program be renamed): an AI client that starts its relay
// again when it ends (VS Code on its next tool call) would have it hold those
// files again at once, and with --start-winshun start Win顺 again as well. The
// new relay is installed last ([Files]), under the old name, where the client
// finds it.
procedure MoveRelayAside();
var
  Relay: String;
begin
  Relay := ExpandConstant('{app}\WinShunMcp.exe');
  if RelayMoved <> '' then
    exit;
  // An earlier Setup ended (killed, power cut) between the rename and the
  // new relay: the .old is the only relay there is, put it back first (GitHub #97).
  if not FileExists(Relay) and FileExists(Relay + '.old') then
    if RenameFile(Relay + '.old', Relay) then
      Log('The MCP relay an earlier Setup left aside was put back')
    else
      Log('The MCP relay an earlier Setup left aside could not be put back');
  if not FileExists(Relay) then
    exit;
  DeleteFile(Relay + '.old'); // an earlier Setup's, if it could not go then
  if RenameFile(Relay, Relay + '.old') then
    RelayMoved := Relay + '.old'
  else
    Log('The MCP relay could not be moved aside');
end;

var
  ClosedWinShun: Boolean; // CloseWinShun found Win顺 running

// Asks a running WinShun to quit, the way it saves its index first: with
// --quit through the installed copy, or WM_CLOSE to its message window (a
// portable copy elsewhere; WinShun 0.2.1 and later). One still running after
// a while is ended.
// First the MCP relays (WinShunMcp.exe): an AI client (Claude, Cursor, VS
// Code) keeps one running for as long as it is open, with Qt6Core.dll and
// msvcp140.dll from the install folder loaded, so those could not be
// replaced (DeleteFile, code 5). Ended before WinShun, too: one would start
// WinShun again through its autostart task when a tool is called. Moved
// aside first, so that it cannot be started again (MoveRelayAside).
// Then the taskbar button's WinShunSearch.exe, which may be waiting to start
// WinShun for a click: it would, and it keeps its own file open meanwhile.
// False when one is still running: as administrator, from a Setup without
// those rights (GitHub #68), neither --quit nor taskkill can end it, and its
// files (the fonts, the DLLs) could not be replaced. The user is asked to
// quit it then.
function CloseWinShun(Exe: String): Boolean;
var
  ResultCode, Waited: Integer;
begin
  Result := True;
  MoveRelayAside();
  // Only this {app}'s own, by id (GitHub #97): not another user's, a portable copy's.
  if EndOurProcesses('WinShunMcp.exe') then
    Sleep(500); // for the files to be let go
  EndOurProcesses('WinShunSearch.exe');
  if not WinShunRunning() then
    exit;
  ClosedWinShun := True;
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

// Setup stopped before the new files: the relay goes back under its name.
procedure PutRelayBack();
var
  Relay: String;
begin
  if RelayMoved = '' then
    exit;
  Relay := Copy(RelayMoved, 1, Length(RelayMoved) - Length('.old'));
  if not FileExists(Relay) and RenameFile(RelayMoved, Relay) then
    RelayMoved := '';
end;

// The relay MoveRelayAside put aside, once the new one is in (or uninstalled).
// Installing, it goes only when the new relay is there (it may have been
// skipped, "Skip this file" after an error): otherwise the AI tools would be
// left with none, and the old one goes back (GitHub #97). One that an AI tool
// still runs (as administrator, from a Setup without those rights) stays
// until it ends: it goes at the next restart (an all-users Setup; deleting at
// the restart takes administrator rights), or with the next Setup or the
// uninstall ([UninstallDelete]).
procedure RemoveMovedRelay(Installing: Boolean);
begin
  if RelayMoved = '' then
    exit;
  if Installing and not FileExists(Copy(RelayMoved, 1, Length(RelayMoved) - Length('.old'))) then
  begin
    Log('The new MCP relay is not there, the old one stays');
    PutRelayBack();
    exit;
  end;
  if FileExists(RelayMoved) and not DeleteFile(RelayMoved) then
  begin
    if IsAdminInstallMode then
      MoveFileEx(RelayMoved, 0, MOVEFILE_DELAY_UNTIL_REBOOT)
    else
      Log('The old MCP relay is still in use; the next Setup or the uninstall removes it');
  end;
  RelayMoved := '';
end;

var
  FilesInstalled: Boolean; // the new files are in (ssPostInstall)

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
    FilesInstalled := True;
    RemoveTapCopies();
    RemoveMovedRelay(True);
  end;
  // After [Run] re-registered the service from {app}: a portable copy's old
  // service folder is now stale.
  if CurStep = ssDone then
    RemoveServiceFolder();
  if (CurStep = ssDone) and not WizardSilent() and
     ((WizardForm.RunList.Items.Count = 0) or not WizardForm.RunList.Checked[0]) then
    Exec(ExpandConstant('{app}\{#AppExe}'), '--quit', '', SW_HIDE, ewNoWait, ResultCode);
end;

var
  ServiceStopped: Boolean; // Setup stopped the index service, and has not started it again

// Stops the index service (the service "WinShun"), so that its file can be
// replaced; waits up to 15 s for it to stop. [Run] starts it again from the
// new files. Nothing to do when it is not installed (a first install).
// Starting or stopping just then (1061, it takes no stop meanwhile), it is
// asked again while waited for. Its process and its helpers in the user's
// session may outlive "STOPPED" by a moment: FilesInUse waits for those.
// Remembered (ServiceStopped): when Setup ends without installing, the
// service is started again (StartIndexService), for nothing else would, and
// until the next restart Win顺 would walk whole disks without it (GitHub #97).
procedure StopIndexService();
var
  ResultCode, Waited: Integer;
begin
  if not Exec(ExpandConstant('{sys}\sc.exe'), 'stop WinShun', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    exit;
  if (ResultCode <> 0) and (ResultCode <> 1061) then
    exit; // not installed (1060), or not running (1062)
  ServiceStopped := True;
  Waited := 0;
  while (Waited < 15000) and Exec(ExpandConstant('{cmd}'), '/C sc query WinShun | find "STOPPED"', '', SW_HIDE,
          ewWaitUntilTerminated, ResultCode) and (ResultCode <> 0) do
  begin
    Sleep(500);
    Waited := Waited + 500;
    if Waited mod 2000 = 0 then
      Exec(ExpandConstant('{sys}\sc.exe'), 'stop WinShun', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;

// The service Setup stopped goes on, for a Setup that did not install: the
// old version is as it was. Not waited for. Nothing when Setup did not stop it.
procedure StartIndexService();
var
  ResultCode: Integer;
begin
  if not ServiceStopped then
    exit;
  ServiceStopped := False;
  if Exec(ExpandConstant('{sys}\sc.exe'), 'start WinShun', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Log(Format('The index service was started again (sc start: %d)', [ResultCode]))
  else
    Log('The index service could not be started again: ' + SysErrorMessage(ResultCode));
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

// ---- What still holds Win顺's files (GitHub #97) -----------------------------
// Restart Manager, Windows' own "files in use" check: what still has a file
// of {app} open once Win顺 and its relays were closed (a relay an AI tool runs
// as administrator, a program still ending) is named, instead of Setup
// stopping at the first such file with "DeleteFile failed; code 5".

type
  TRmUniqueProcess = record
    ProcessId: DWORD;
    StartTimeLow: DWORD;
    StartTimeHigh: DWORD;
  end;
  TRmProcessInfo = record
    Process: TRmUniqueProcess;
    AppName: array[0..255] of Char;
    ServiceShortName: array[0..63] of Char;
    AppType: Integer;
    AppStatus: DWORD;
    SessionId: DWORD;
    Restartable: BOOL;
  end;
  TRmProcessInfos = record
    Items: array[0..255] of TRmProcessInfo; // more than 32: an AI tool and its helpers add up
  end;

const
  RmBuffer = 256; // processes TRmProcessInfos holds

function RmStartSession(var Session: DWORD; Flags: DWORD; Key: String): DWORD;
  external 'RmStartSession@rstrtmgr.dll stdcall delayload';
// All the files in one call (Microsoft's advice), as the array of names it takes.
function RmRegisterFiles(Session, Count: DWORD; const FileNames: array of String; AppCount, Apps, ServiceCount, Services: DWORD): DWORD;
  external 'RmRegisterResources@rstrtmgr.dll stdcall delayload';
function RmGetList(Session: DWORD; var Needed, Count: DWORD; var Infos: TRmProcessInfos; var Reasons: DWORD): DWORD;
  external 'RmGetList@rstrtmgr.dll stdcall delayload';
function RmEndSession(Session: DWORD): DWORD;
  external 'RmEndSession@rstrtmgr.dll stdcall delayload';

function CharsText(const Chars: array of Char): String;
var
  I: Integer;
begin
  Result := '';
  for I := 0 to GetArrayLength(Chars) - 1 do
  begin
    if Chars[I] = #0 then
      break;
    Result := Result + Chars[I];
  end;
end;

// The programs and DLLs under Folder, and the fonts (Win顺 keeps those open).
procedure CollectFiles(Folder: String; Files: TStringList);
var
  Find: TFindRec;
  Name, Ext: String;
begin
  if FindFirst(Folder + '\*', Find) then
    try
      repeat
        if (Find.Name = '.') or (Find.Name = '..') then
          continue;
        Name := Folder + '\' + Find.Name;
        Ext := Lowercase(ExtractFileExt(Name));
        if (Find.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
          CollectFiles(Name, Files)
        else if (Ext = '.exe') or (Ext = '.dll') or (Ext = '.ttf') then
          Files.Add(Name);
      until not FindNext(Find);
    finally
      FindClose(Find);
    end;
end;

// What has Win顺's files open: a line ("· name") for each program, the AI
// tool's name for a relay; '' when nothing does. When Restart Manager itself
// fails (no session, no answer) it is logged and '' is returned too: Setup
// then goes on as it did before this check, and a file still in use is met by
// Inno Setup's own handling (it retries, then asks Retry / Skip / Cancel,
// naming the file), so nothing is lost but the nicer list.
function FilesInUse(): String;
var
  Session, Needed, Count, Reasons, Status: DWORD;
  Infos: TRmProcessInfos;
  Files: TStringList;
  Names: array of String;
  I, Pid, Parent: Integer;
  Key, Name, Image, Path, Command, Tool, Line: String;
begin
  Result := '';
  if not DirExists(ExpandConstant('{app}')) then
    exit;
  Files := TStringList.Create;
  try
    try
      CollectFiles(ExpandConstant('{app}'), Files);
      if Files.Count = 0 then
        exit;
      SetArrayLength(Names, Files.Count);
      for I := 0 to Files.Count - 1 do
        Names[I] := Files[I];
      Key := StringOfChar(' ', 64); // CCH_RM_SESSION_KEY + 1
      Status := RmStartSession(Session, 0, Key);
      if Status <> 0 then
      begin
        Log(Format('Restart Manager: RmStartSession failed (%d)', [Status]));
        exit;
      end;
      try
        Status := RmRegisterFiles(Session, Files.Count, Names, 0, 0, 0, 0);
        if Status <> 0 then
        begin
          Log(Format('Restart Manager: RmRegisterResources failed (%d)', [Status]));
          exit;
        end;
        Needed := 0;
        Count := RmBuffer;
        Reasons := 0;
        Status := RmGetList(Session, Needed, Count, Infos, Reasons);
        if Status <> 0 then
        begin
          // 234 (ERROR_MORE_DATA): more than RmBuffer programs, which Needed
          // says; no list to show then.
          Log(Format('Restart Manager: RmGetList failed (%d), %d in use', [Status, Needed]));
          exit;
        end;
        for I := 0 to Count - 1 do
        begin
          Pid := Infos.Items[I].Process.ProcessId;
          Name := CharsText(Infos.Items[I].AppName);
          Image := '';
          if ProcessInfo(Pid, Image, Path, Command, Parent) and (CompareText(Image, 'svchost.exe') = 0) then
          begin
            // A Windows service glancing at a program just started or ended (the
            // inventory service): it lets go by itself, and Setup retries a file a
            // few times anyway. Nothing the user could close.
            Log(Format('In use by process %d (%s), not listed: %s', [Pid, Image, Name]));
            continue;
          end;
          if CompareText(Image, 'WinShunMcp.exe') = 0 then
          begin
            Tool := AiToolOf(Pid);
            if Tool = '' then
              Tool := CustomMessage('AiTool');
            Name := FmtMessage(CustomMessage('FilesInUseAi'), [Tool]);
          end;
          Log(Format('In use by process %d (%s): %s', [Pid, Image, Name]));
          Line := '· ' + Name + #13#10;
          if Pos(Line, Result) = 0 then
            Result := Result + Line;
        end;
      finally
        RmEndSession(Session);
      end;
    except
      Log('Restart Manager: ' + GetExceptionMessage);
    end;
  finally
    Files.Free;
  end;
end;

// An AI tool using Win顺 is cut off when its relay is ended, and cannot start
// it again until the new one is in (GitHub #97): the user says whether that
// is all right now. Esc and the close box mean later.
function InstallDespiteAiTools(Tools: String): Boolean;
begin
  Result := TaskDialogMsgBox(CustomMessage('AiInUseTitle'), FmtMessage(CustomMessage('AiInUse'), [Tools]), mbInformation,
    MB_OKCANCEL, [CustomMessage('AiInstallNow') + #13#10 + CustomMessage('AiInstallNowNote'),
    CustomMessage('AiInstallLater') + #13#10 + CustomMessage('AiInstallLaterNote')], 0) = IDOK;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Tools, Holders: String;
  Tries, ResultCode: Integer;
begin
  // AI tools using Win顺 are asked about before anything is closed (GitHub
  // #97). A silent Setup (an update run by a script) goes on, as before.
  Tools := AiTools();
  if Tools <> '' then
    Log('Win顺 is in use by AI tools: ' + Tools);
  if (Tools <> '') and not WizardSilent() and not InstallDespiteAiTools(Tools) then
  begin
    Result := CustomMessage('AiLater');
    exit;
  end;
  // First, so that --quit through the installed copy can start (GitHub #68).
  RemoveRunAsAdmin(HKCU);
  if IsAdminInstallMode then
    RemoveRunAsAdmin(HKLM64);
  // Closed, then checked that nothing holds the files any more. Retry closes
  // everything again (a relay started again meanwhile goes too). A program
  // ended just now may take a moment to let go: looked at once more first. A
  // silent Setup has nobody to ask: it waits a while, then stops, the old
  // version left as it was.
  Tries := 0;
  repeat
    // The installed copy if there is one; a portable copy is ended.
    if not CloseWinShun(ExpandConstant('{app}\{#AppExe}')) then
    begin
      PutRelayBack();
      StartIndexService(); // stopped by an earlier round of this loop
      Result := CustomMessage('NotClosed');
      exit;
    end;
    if IsAdminInstallMode then
      StopIndexService(); // there is no service when installed for the user alone
    Holders := FilesInUse();
    Tries := Tries + 1;
    if Holders = '' then
      break;
    // 90 s when silent: a WinShun.exe whose WebDAV read hung at exit is only
    // let go when WebClient gives the request up (~61 s, see pitfalls.md).
    if (Tries < 2) or (WizardSilent() and (Tries < 90)) then
      Sleep(1000)
    else if WizardSilent() or (TaskDialogMsgBox(CustomMessage('FilesInUseTitle'),
      FmtMessage(CustomMessage('FilesInUse'), [Holders]), mbError, MB_RETRYCANCEL, [], 0) <> IDRETRY) then
      break;
  until False;
  if Holders <> '' then
  begin
    PutRelayBack();
    // Nothing was replaced: the old version goes on, with its service (first,
    // for WinShun looks for it when it starts), back in the tray.
    StartIndexService();
    if ClosedWinShun and not WinShunRunning() then
      ExecAsOriginalUser(ExpandConstant('{app}\{#AppExe}'), '--background', '', SW_SHOWNORMAL, ewNoWait, ResultCode);
    Result := CustomMessage('FilesNotFreed');
    exit;
  end;
  // This copy's autostart task of old (GitHub #21): registered elevated by
  // the version before, and by whoever answered UAC then, which for a
  // standard user was an administrator, whose task that user can neither
  // read nor replace. Removed here (elevated); [Run] has the user who
  // started Setup write their own, with normal rights.
  HadAutostart := RemoveAutostart();
  Result := '';
end;

// Setup ended before the new files were in (cancelled, failed): the relay
// MoveRelayAside put aside goes back; once they are in, it goes. Kept when
// neither is there under its name, so that the AI tools are never left with none.
procedure DeinitializeSetup();
begin
  // The index service Setup stopped, when the new version did not come in
  // (cancelled, failed, rolled back); [Run] starts it otherwise.
  if not FilesInstalled then
    StartIndexService();
  // Nothing put aside, nothing to do. Also the only case where {app} may not
  // be set yet (Cancel on the folder page of a first install, or a refusal in
  // InitializeSetup): expanding it then raises an internal error.
  if RelayMoved = '' then
    exit;
  PutRelayBack();
  RemoveMovedRelay(True);
end;

// Win+V back to Windows' clipboard history, and Win+S (with Win+Shift+S) to
// its search, if WinShun had taken them over: the letter it put into
// Explorer's DisabledHotkeys goes (Explorer reads that when it starts). A
// letter there without WinShun having taken the key belongs to someone else.
procedure GiveBackKey(Section, Name, Key: String);
var
  Keys, Recorded: String;
  I: Integer;
  Ours: Boolean;
begin
  // Older versions kept no record of the letters they set: the ini says.
  Ours := CompareText(GetIniString(Section, Name, 'false', ExpandConstant('{userappdata}\WinShun\WinShun.ini')), 'true') = 0;
  if RegQueryStringValue(HKCU, 'Software\WinShun', 'ExplorerHotkeys', Recorded) then
    Ours := Ours or (Pos(Key, Recorded) > 0);
  if not Ours then
    exit;
  if not RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced', 'DisabledHotkeys', Keys) then
    exit;
  for I := Length(Keys) downto 1 do
    if Keys[I] = Key then
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

// Explorer's right-click "用 Win顺 搜索此处" and "粘贴为文件"
// (src/app/platform/ShellMenu.cpp), when they run this copy's
// WinShunSearch.exe: a portable copy's stay.
procedure RemoveExplorerMenu();
var
  Keys: array[0..3] of String;
  Command: String;
  I: Integer;
begin
  Keys[0] := 'Software\Classes\Directory\shell\WinShun.SearchHere';
  Keys[1] := 'Software\Classes\Directory\Background\shell\WinShun.SearchHere';
  Keys[2] := 'Software\Classes\Drive\shell\WinShun.SearchHere';
  Keys[3] := 'Software\Classes\Directory\Background\shell\WinShun.PasteFile';
  for I := 0 to 3 do
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
    // Directory Opus's double click on blank space, if Win顺 set it to run this copy.
    Exec(ExpandConstant('{app}\WinShunSearch.exe'), '--opus-menu-off', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    GiveBackKey('Clipboard', 'WinV', 'V');
    GiveBackKey('Taskbar', 'WinS', 'S');
    RegDeleteValue(HKCU, 'Software\WinShun', 'ExplorerHotkeys');
    RegDeleteKeyIfEmpty(HKCU, 'Software\WinShun');
  end;
  // Old versions' copies of WinShunTap.dll (RemoveTapCopies); the folder
  // itself only when nothing else is in it (the index service's log).
  if CurUninstallStep = usPostUninstall then
  begin
    // The relay CloseWinShun moved aside, and the folder it kept, now empty.
    if RelayMoved <> '' then
    begin
      RemoveMovedRelay(False);
      RemoveDir(ExpandConstant('{app}'));
    end;
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
