#include "ContentOpen.h"
#include "Shell.h"
#include <QFileInfo>
#include <shlwapi.h>
#include <thread>
using namespace Qt::StringLiterals;
namespace ws::contentopen {
std::optional<QString> arguments(const QString& program, const QString& path, int line)
{
    if (line <= 0 || path.contains(u'"') || path.contains(QChar::Null)) return {};
    const QString name = QFileInfo(program).fileName().toLower();
    if (name == u"code.exe" || name == u"code-insiders.exe") return u"--goto \"%1:%2\""_s.arg(path).arg(line);
    if (name == u"notepad++.exe") return u"-n%1 \"%2\""_s.arg(line).arg(path);
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
