#include "ContentOpen.h"
#include "Shell.h"
#include <QFileInfo>
#include <QStringList>
#include <shlwapi.h>
#include <thread>
using namespace Qt::StringLiterals;
namespace ws::contentopen {
std::optional<QString> arguments(const QString& program, const QString& path, int line)
{
    if (line <= 0 || path.contains(u'"') || path.contains(QChar::Null)) return {};
    const QString name = QFileInfo(program).fileName().toLower();
    // VS Code and the editors built on it (--goto), Sublime Text, Zed: "path:line".
    static const QStringList vsCode { u"code.exe"_s, u"code - insiders.exe"_s, u"code-insiders.exe"_s, u"vscodium.exe"_s,
                                     u"cursor.exe"_s, u"windsurf.exe"_s };
    static const QStringList colon { u"sublime_text.exe"_s, u"subl.exe"_s, u"zed.exe"_s };
    static const QStringList jetBrains { u"idea64.exe"_s, u"idea.exe"_s, u"pycharm64.exe"_s, u"webstorm64.exe"_s, u"phpstorm64.exe"_s,
                                         u"clion64.exe"_s, u"rider64.exe"_s, u"goland64.exe"_s, u"rubymine64.exe"_s,
                                         u"datagrip64.exe"_s, u"dataspell64.exe"_s, u"rustrover64.exe"_s, u"studio64.exe"_s,
                                         u"aqua64.exe"_s };
    static const QStringList slashG { u"notepad4.exe"_s, u"notepad3.exe"_s, u"notepad2.exe"_s, u"notepad2-mod.exe"_s };
    if (vsCode.contains(name)) return u"--goto \"%1:%2\""_s.arg(path).arg(line);
    if (colon.contains(name)) return u"\"%1:%2\""_s.arg(path).arg(line);
    if (jetBrains.contains(name)) return u"--line %1 \"%2\""_s.arg(line).arg(path);
    if (slashG.contains(name)) return u"/g %1 \"%2\""_s.arg(line).arg(path);
    if (name == u"notepad++.exe") return u"-n%1 \"%2\""_s.arg(line).arg(path);
    if (name == u"emeditor.exe") return u"/l %1 \"%2\""_s.arg(line).arg(path);
    if (name == u"gvim.exe") return u"+%1 \"%2\""_s.arg(line).arg(path);
    return {};
}
void open(const QString& path, int line, std::function<void(bool)> done)
{
    std::thread([path, line, done = std::move(done)] () mutable {
        const QString extension = u'.' + QFileInfo(path).suffix();
        wchar_t program[32768] {};
        DWORD length = static_cast<DWORD>(std::size(program));
        if (line > 0 && SUCCEEDED(::AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE,
                reinterpret_cast<LPCWSTR>(extension.utf16()), L"open", program, &length))) {
            const QString executable = QString::fromWCharArray(program);
            if (const auto args = arguments(executable, path, line)) {
                if (shell::startUnelevated(executable.toStdWString(), args->toStdWString()).valid()) {
                    if (done) done(true);
                    return;
                }
            }
        }
        shell::open(path, false, std::move(done));
    }).detach();
}
}
