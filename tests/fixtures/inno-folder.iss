; A real Inno folder picker for tst_filedialog. Nothing is installed.
; Compile with ISCC, then set WINSHUN_TEST_INNO_SETUP to the resulting exe.
[Setup]
AppName=WinShun Dialog Test
AppVersion=1
DefaultDirName={tmp}\WinShunDialogTest
DisableWelcomePage=yes
DisableDirPage=no
PrivilegesRequired=lowest
Uninstallable=no
OutputBaseFilename=inno-folder-test
OutputDir=.

[Code]
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := CurPageID <> wpReady;
end;

procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);
begin
  Cancel := True;
  Confirm := False;
end;
