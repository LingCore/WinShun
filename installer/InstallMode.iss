[CustomMessages]
; (A section first: #included after a [Code] section, as the probe does,
; these comment lines would be read as Pascal.)
;
; Which install mode Setup takes, for all users or for the user alone, when the
; command line does not say (#included by WinShun.iss, and by
; tests/InstallModeProbe.iss, which runs this decision without installing).
;
; Inno Setup's own dialog for it (PrivilegesRequiredOverridesAllowed=dialog)
; recommends one mode for everybody: the script's PrivilegesRequired, whether or
; not the account is an administrator, and it comes before any [Code]. So
; Setup starts in the user's own mode (PrivilegesRequired=lowest, the switches
; only: commandline) and decides here, in InitializeSetup:
;   1. /ALLUSERS or /CURRENTUSER given (the in-app updater always passes one):
;      that mode, nothing asked. Inno itself asks for administrator rights for
;      /ALLUSERS.
;   2. Installed before in one mode only (the uninstall entry in HKLM or in
;      HKCU): that mode again, as Inno's UsePreviousPrivileges did.
;   3. Otherwise asked, the mode that suits the account first: all users for an
;      administrator (who confirms the UAC prompt with one click), for me only
;      for a standard account (which would need an administrator's password).
;      A silent Setup takes that first choice without asking.
; All users chosen in this process (which runs in the user's own mode): Setup
; runs itself again with /ALLUSERS and ends with that run's exit code. Started
; with CreateProcess, not the runas verb: Inno elevates the new run itself,
; and keeps an unelevated process for runasoriginaluser ([Run]: WinShun and
; its autostart task run as the user, GitHub #21). When it gets no
; administrator rights (UAC cancelled, no password), installing for the user
; alone is offered instead.
;
; Before the #include: nothing. InstallModeChosen(UninstallKey) is called first
; in InitializeSetup; False: end Setup (it may also end the process itself,
; with the exit code of the run it started).

; The texts of Inno's own dialog for this (PrivilegesRequiredOverride* in
; Default.isl / ChineseSimplified.isl), worded for Win顺. Text1 when all users
; comes first, Text2 when "only me" does.
english.InstallModeTitle=Select install mode
chinesesimplified.InstallModeTitle=选择安装模式
english.InstallModeText1=%1 can be installed for all users or for you only.%n%nAll users (needs an administrator): with the index service, which indexes whole disks and is the fastest.%nOnly me (no administrator needed): without the service, only the folders you choose are indexed. Choose this on a work computer.
english.InstallModeText2=%1 can be installed for you only or for all users.%n%nAll users (needs an administrator): with the index service, which indexes whole disks and is the fastest.%nOnly me (no administrator needed): without the service, only the folders you choose are indexed. Choose this on a work computer.
english.InstallModeAllUsers=Install for &all users (needs an administrator)
english.InstallModeCurrentUser=Install for &me only (no administrator needed)
chinesesimplified.InstallModeText1=%1 可以为所有用户安装，也可以只为您安装。%n%n为所有用户安装：带索引服务，索引整个磁盘，最快。%n只为我安装：不装服务，只索引你选的文件夹。公司电脑选这个。
chinesesimplified.InstallModeText2=%1 可以只为您安装，也可以为所有用户安装。%n%n为所有用户安装：带索引服务，索引整个磁盘，最快。%n只为我安装：不装服务，只索引你选的文件夹。公司电脑选这个。
chinesesimplified.InstallModeAllUsers=为所有用户安装(&A)（需要管理员）
chinesesimplified.InstallModeCurrentUser=只为我安装(&M)（不需要管理员）
; All users was chosen, but the run with administrator rights did not start.
english.InstallModeNoAdmin=Setup did not get administrator rights, so it cannot install for all users.%n%nInstall %1 for you only instead (no administrator needed)?
chinesesimplified.InstallModeNoAdmin=没有拿到管理员权限，不能为所有用户安装。%n%n改为只为您安装 %1 吗？（不需要管理员）

[Code]
// ---- Install mode: for all users or for me only ------------------------------

const
  TOKEN_QUERY = $0008;
  TokenElevationTypeInfo = 18;     // TOKEN_INFORMATION_CLASS TokenElevationType
  TokenElevationTypeDefault = 1;   // no split token: a standard account (or UAC off)
  TokenElevationTypeFull = 2;      // elevated
  TokenElevationTypeLimited = 3;   // an administrator's filtered token: elevates with a click
  // The decision (InstallModeDecision).
  imSwitch = 0;          // /ALLUSERS or /CURRENTUSER on the command line
  imAllUsers = 1;        // installed for all users before
  imCurrentUser = 2;     // installed for the user alone before
  imAskAllUsers = 3;     // ask, all users first
  imAskCurrentUser = 4;  // ask, "only me" first
  // Passed to the run started for all users, which creates this file as it
  // starts: it ran (and its exit code is the install's), so no administrator
  // rights were refused.
  RelaunchParam = 'WSRELAUNCH';

function InstallModeOpenProcessToken(Process: THandle; Access: DWORD; var Token: THandle): BOOL;
  external 'OpenProcessToken@advapi32.dll stdcall';
function InstallModeGetTokenInformation(Token: THandle; InfoClass: Integer; var Info: DWORD; Size: DWORD; var Returned: DWORD): BOOL;
  external 'GetTokenInformation@advapi32.dll stdcall';
function InstallModeGetCurrentProcess(): THandle;
  external 'GetCurrentProcess@kernel32.dll stdcall';
function InstallModeCloseHandle(Handle: THandle): BOOL;
  external 'CloseHandle@kernel32.dll stdcall';
procedure InstallModeExitProcess(ExitCode: Cardinal);
  external 'ExitProcess@kernel32.dll stdcall';

var
  InstallModeTrace: String; // what was decided and why, one line each (Setup's log, the probe)

// tests/InstallModeProbe.iss defines, before its #include: ProbeNote(Line)
// (each line to its log as it comes), ProbeAnswer (IDYES, IDNO, IDCANCEL for
// the dialog, 0 to show it) and ProbeRefuseRelaunch (non-zero: play a run
// for all users that got no administrator rights, ending with that code).
procedure InstallModeNote(Line: String);
begin
  Log('Install mode: ' + Line);
  InstallModeTrace := InstallModeTrace + Line + #13#10;
#ifdef InstallModeProbe
  ProbeNote(Line);
#endif
end;

// Whether this account installs for all users without anybody else's
// password: running elevated, or an administrator's split token (UAC asks
// for a click only). A standard account (TokenElevationTypeDefault without
// administrator rights) would need an administrator to type theirs. Kind
// says which ('elevated', 'administrator', 'standard'); when the token
// cannot be read it is 'unknown', taken as an administrator: all users first,
// as before.
function AccountCanElevate(var Kind: String): Boolean;
var
  Token: THandle;
  Elevation, Returned: DWORD;
begin
  Result := True;
  if IsAdmin() then
  begin
    Kind := 'elevated';
    exit;
  end;
  Kind := 'unknown';
  if not InstallModeOpenProcessToken(InstallModeGetCurrentProcess(), TOKEN_QUERY, Token) then
    exit;
  try
    if InstallModeGetTokenInformation(Token, TokenElevationTypeInfo, Elevation, 4, Returned) then
      if Elevation = TokenElevationTypeDefault then
      begin
        Kind := 'standard';
        Result := False;
      end
      else if (Elevation = TokenElevationTypeLimited) or (Elevation = TokenElevationTypeFull) then
        Kind := 'administrator';
  finally
    InstallModeCloseHandle(Token);
  end;
end;

// Where Win顺 is installed now: 'allusers' (an uninstall entry in HKLM, either
// registry view), 'currentuser' (in HKCU), 'both' or 'none'.
function PreviousInstallMode(UninstallKey: String): String;
var
  AllUsers, CurrentUser: Boolean;
begin
  AllUsers := RegKeyExists(HKLM64, UninstallKey) or RegKeyExists(HKLM32, UninstallKey);
  CurrentUser := RegKeyExists(HKCU, UninstallKey);
  if AllUsers and CurrentUser then
    Result := 'both'
  else if AllUsers then
    Result := 'allusers'
  else if CurrentUser then
    Result := 'currentuser'
  else
    Result := 'none';
end;

function ParamIs(I: Integer; Name: String): Boolean;
begin
  Result := CompareText(ParamStr(I), Name) = 0;
end;

function ParamStarts(I: Integer; Prefix: String): Boolean;
begin
  Result := CompareText(Copy(ParamStr(I), 1, Length(Prefix)), Prefix) = 0;
end;

function ModeSwitchGiven(): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if ParamIs(I, '/ALLUSERS') or ParamIs(I, '/CURRENTUSER') then
      Result := True;
end;

// The decision, before anything is shown or started (one of the im* above).
function InstallModeDecision(UninstallKey: String): Integer;
var
  Previous, Account: String;
  CanElevate: Boolean;
begin
  if ModeSwitchGiven() then
  begin
    InstallModeNote('switch on the command line; admin install mode = ' + IntToStr(Ord(IsAdminInstallMode)));
    Result := imSwitch;
    exit;
  end;
  CanElevate := AccountCanElevate(Account);
  Previous := PreviousInstallMode(UninstallKey);
  InstallModeNote('account = ' + Account + ', previous install = ' + Previous);
  if Previous = 'allusers' then
    Result := imAllUsers
  else if Previous = 'currentuser' then
    Result := imCurrentUser
  else if CanElevate then
    Result := imAskAllUsers
  else
    Result := imAskCurrentUser;
  InstallModeNote('decision = ' + IntToStr(Result));
end;

// The two buttons, the mode that suits the account first (Text1 / Text2, as
// in Inno's own dialog); the UAC shield on all users. True: all users.
// Cancel or the close box: Setup ends, Cancelled.
function AskInstallMode(AllUsersFirst: Boolean; var Cancelled: Boolean): Boolean;
var
  Text, First, Second: String;
  Shield, Choice: Integer;
begin
  if AllUsersFirst then
  begin
    Text := FmtMessage(CustomMessage('InstallModeText1'), ['{#SetupSetting("AppName")}']);
    First := CustomMessage('InstallModeAllUsers');
    Second := CustomMessage('InstallModeCurrentUser');
    Shield := IDYES;
  end
  else
  begin
    Text := FmtMessage(CustomMessage('InstallModeText2'), ['{#SetupSetting("AppName")}']);
    First := CustomMessage('InstallModeCurrentUser');
    Second := CustomMessage('InstallModeAllUsers');
    Shield := IDNO;
  end;
  InstallModeNote('dialog: first = ' + First + ', second = ' + Second);
  Choice := 0;
#ifdef InstallModeProbe
  Choice := ProbeAnswer();
#endif
  if Choice = 0 then
    Choice := TaskDialogMsgBox(CustomMessage('InstallModeTitle'), Text, mbConfirmation, MB_YESNOCANCEL, [First, Second], Shield);
  InstallModeNote('dialog answer = ' + IntToStr(Choice));
  Cancelled := (Choice <> IDYES) and (Choice <> IDNO);
  Result := (Choice = IDYES) = AllUsersFirst;
end;

// Arg as one command-line argument again (ParamStr took the quotes off).
function ArgText(Arg: String): String;
begin
  if (Pos(' ', Arg) > 0) or (Pos(#9, Arg) > 0) then
    Result := '"' + Arg + '"'
  else
    Result := Arg;
end;

// This Setup's own arguments, for the run for all users: /LANG kept (the
// language this one shows; ShowLanguageDialog may have asked), and /LOG
// moved to Name.allusers.ext, as this process still writes the one given.
function RelaunchArgs(Marker: String): String;
var
  I: Integer;
  Arg, LogFile: String;
  HasLang: Boolean;
begin
  Result := '';
  HasLang := False;
  for I := 1 to ParamCount do
  begin
    Arg := ParamStr(I);
    if ParamStarts(I, '/' + RelaunchParam + '=') then
      continue;
    if ParamStarts(I, '/LANG=') then
      HasLang := True;
    if ParamStarts(I, '/LOG=') then
    begin
      LogFile := Copy(Arg, Length('/LOG=') + 1, Length(Arg));
      Arg := '/LOG=' + ChangeFileExt(LogFile, '') + '.allusers' + ExtractFileExt(LogFile);
    end;
    Result := Result + ArgText(Arg) + ' ';
  end;
  if not HasLang then
    Result := Result + '/LANG=' + ActiveLanguage + ' ';
  Result := Result + '/ALLUSERS ' + ArgText('/' + RelaunchParam + '=' + Marker);
end;

// Setup again, with /ALLUSERS, waited for. True: it ran (it created the
// marker); ExitCode is then the install's. False: it got no administrator
// rights (Inno ends with 2 when UAC is cancelled), or did not start at all.
function RelaunchForAllUsers(var ExitCode: Integer): Boolean;
var
  Marker, Args: String;
begin
  Marker := ExpandConstant('{tmp}\allusers-started');
  DeleteFile(Marker);
  Args := RelaunchArgs(Marker);
  InstallModeNote('relaunch: ' + ExpandConstant('{srcexe}') + ' ' + Args);
#ifdef InstallModeProbe
  if ProbeRefuseRelaunch() <> 0 then
  begin
    ExitCode := ProbeRefuseRelaunch();
    InstallModeNote('relaunch refused (probe), exit code ' + IntToStr(ExitCode));
    Result := False;
    exit;
  end;
#endif
  // As a whole command line ('>'): Exec refuses Setup's own file by name
  // until the installation has started (error 5), a guard against a Setup
  // that runs itself over and over. This one cannot: the new run has
  // /ALLUSERS, and with a mode switch nothing here runs Setup again.
  if not Exec('>', '"' + ExpandConstant('{srcexe}') + '" ' + Args, ExtractFileDir(ExpandConstant('{srcexe}')),
              SW_SHOWNORMAL, ewWaitUntilTerminated, ExitCode) then
  begin
    InstallModeNote(Format('relaunch did not start: error %d, %s', [ExitCode, SysErrorMessage(ExitCode)]));
    ExitCode := 1;
    Result := False;
    exit;
  end;
  Result := FileExists(Marker);
  InstallModeNote(Format('relaunch ended with %d, ran = %d', [ExitCode, Ord(Result)]));
end;

// Ends this process with the exit code of the run that did the work, so that
// a script waiting for setup.exe sees that one (InitializeSetup returning False
// would always end with 1). Nothing is installed or open here yet; this
// process's temporary folder goes first, as Inno would remove it.
procedure EndLikeRelaunch(ExitCode: Integer);
begin
  InstallModeNote('ending with exit code ' + IntToStr(ExitCode));
  DelTree(ExpandConstant('{tmp}'), True, True, True);
  InstallModeExitProcess(ExitCode);
end;

// The run started for all users says it ran (RelaunchForAllUsers).
procedure NoteRelaunchStarted();
var
  Marker: String;
begin
  Marker := ExpandConstant('{param:' + RelaunchParam + '|}');
  if Marker = '' then
    exit;
#ifdef InstallModeProbe
  if ProbeRefuseRelaunch() <> 0 then
    exit;
#endif
  if not SaveStringToFile(Marker, 'started', False) then
    Log('Install mode: the marker could not be written: ' + Marker);
end;

// First in InitializeSetup. True: go on in the mode Setup is in. False: end
// Setup (asked and cancelled). A run for all users started here ends this
// process itself (EndLikeRelaunch).
function InstallModeChosen(UninstallKey: String): Boolean;
var
  Decision, ExitCode, Answer: Integer;
  AllUsers, Cancelled: Boolean;
begin
  Result := True;
  NoteRelaunchStarted();
  Decision := InstallModeDecision(UninstallKey);
  if (Decision = imSwitch) or (Decision = imCurrentUser) then
    exit;
  if Decision = imAllUsers then
    AllUsers := True
  else if WizardSilent() then
  begin
    AllUsers := Decision = imAskAllUsers; // silent: the first button, unasked
    InstallModeNote('silent: all users = ' + IntToStr(Ord(AllUsers)));
  end
  else
  begin
    AllUsers := AskInstallMode(Decision = imAskAllUsers, Cancelled);
    if Cancelled then
    begin
      Result := False;
      exit;
    end;
  end;
  if not AllUsers then
    exit; // this process is in that mode already
  if RelaunchForAllUsers(ExitCode) then
    EndLikeRelaunch(ExitCode);
  // No administrator rights: for the user alone instead, if there is no
  // all-users copy (InitializeSetup would refuse to add one beside it).
  if (Decision <> imAllUsers) and not WizardSilent() then
  begin
    Answer := 0;
#ifdef InstallModeProbe
    Answer := ProbeAnswer();
#endif
    if Answer = 0 then
      Answer := MsgBox(FmtMessage(CustomMessage('InstallModeNoAdmin'), ['{#SetupSetting("AppName")}']), mbConfirmation, MB_YESNO);
    InstallModeNote('no administrator rights, for the user alone instead: ' + IntToStr(Ord(Answer = IDYES)));
    if Answer = IDYES then
      exit;
  end;
  EndLikeRelaunch(ExitCode);
end;
