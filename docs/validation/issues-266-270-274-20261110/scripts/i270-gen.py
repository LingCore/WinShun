# Builds a tiny uninstaller from the REAL installer/WinShun.iss pieces of #270
# (extracted verbatim, not retyped), for a live test of the give-back + restart.
import sys
src, out, app, outdir = sys.argv[1:5]
lines = open(src, encoding='utf-8-sig').read().split('\n')
def block(start):
    i = next(n for n, l in enumerate(lines) if l.startswith(start))
    j = next(n for n in range(i, len(lines)) if lines[n] == 'end;')
    return '\n'.join(lines[i:j + 1])
msgs = [l for l in lines if l.startswith(('english.RestartExplorer=', 'chinesesimplified.RestartExplorer='))]
assert len(msgs) == 2
i = next(n for n, l in enumerate(lines) if l.strip().startswith("GaveV := GiveBackKey("))
j = next(n for n in range(i, len(lines)) if lines[n].strip() == "RestartExplorerFor('Win+S');")
calls = '\n'.join(lines[i:j + 1])
iss = f'''#define AppExe "WinShun.exe"
[Setup]
AppId={{{{3D2E9B10-0270-4A55-9C1E-270270270270}}
AppName=ws270 uninstall test
AppVersion=1.0
DefaultDirName={app}
DirExistsWarning=no
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=admin
OutputDir={outdir}
OutputBaseFilename=ws270-tiny-setup
ShowLanguageDialog=no
LanguageDetectionMethod=uilanguage

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "F:\电脑便捷工具\installer\ChineseSimplified.isl"

[CustomMessages]
{chr(10).join(msgs)}

[Code]
{block('function GiveBackKey(')}

{block('procedure RestartExplorerFor(')}

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  GaveV, GaveS: Boolean;
begin
  if CurUninstallStep = usUninstall then
  begin
{calls}
  end;
end;
'''
open(out, 'w', encoding='utf-8-sig').write(iss)
print(iss)
