#include "CliSetup.h"

#include "SettingsEditor.h"
#include "platform/SettingBroadcast.h"

#include <QCoreApplication>
#include <QPointer>

#include <thread>

using namespace Qt::StringLiterals;

namespace ws {

CliSetup::CliSetup(SettingsEditor* editor, QObject* parent)
    : QObject(parent)
    , m_editor(editor)
{
    m_state = cli::toVariant({});
    m_state.insert(u"checked"_s, false);
}

QString CliSetup::example() const
{
    return tr("ws 报告");
}

void CliSetup::refresh()
{
    const int look = ++m_looks;
    if (m_probe) {
        apply(look, m_probe());
        return;
    }
    std::thread([guard = QPointer<CliSetup>(this), look] {
        const cli::Status status = cli::probe();
        // Asked whether the window is still there on the GUI thread, where it can go.
        QMetaObject::invokeMethod(qApp, [guard, look, status] {
            if (guard)
                guard->apply(look, status);
        }, Qt::QueuedConnection);
    }).detach();
}

void CliSetup::apply(int look, const cli::Status& status)
{
    if (look != m_looks)
        return; // a newer look is on its way
    m_state = cli::toVariant(status);
    m_state.insert(u"checked"_s, true);
    emit stateChanged();
}

void CliSetup::enable()
{
    if (m_busy)
        return;
    // Wanting the command line is wanting the connection: the switch goes on with it.
    const bool switchWasOff = m_editor && !m_editor->mcp();
    if (switchWasOff)
        m_editor->setMcp(true);
    const QString bin = cli::binFolder();
    if (cli::wsExe(bin).isEmpty()) {
        setMessage((switchWasOff ? tr("连接已打开，但没找到 ws.exe（应在 %1）。重新安装 Win顺 就有了。")
                                 : tr("没找到 ws.exe（应在 %1）。重新安装 Win顺 就有了。")).arg(bin));
        refresh();
        return;
    }
    if (m_state.value(u"onPath"_s).toBool()) {
        setMessage(tr("命令行已启用。在新打开的终端里输入 ws 试试。"));
        refresh();
        return;
    }
    changePath(true);
}

void CliSetup::removeFromPath()
{
    if (!m_busy)
        changePath(false);
}

void CliSetup::changePath(bool add)
{
    setBusy(true);
    if (m_probe) { // the preview: nothing is written
        finishChange(add, 0);
        return;
    }
    const QString bin = cli::binFolder();
    std::thread([guard = QPointer<CliSetup>(this), bin, add] {
        const DWORD error = add ? cli::addToUserPath(bin) : cli::removeFromUserPath(bin);
        // New terminals read the PATH from the registry when they start; the
        // ones that are open learn of it from this (Explorer passes it on to
        // the programs it starts).
        if (error == 0)
            settingbroadcast::send(settingbroadcast::Area::Environment);
        QMetaObject::invokeMethod(qApp, [guard, add, error] {
            if (guard)
                guard->finishChange(add, error);
        }, Qt::QueuedConnection);
    }).detach();
}

void CliSetup::finishChange(bool add, unsigned long error)
{
    setBusy(false);
    if (error == 0)
        setMessage(add ? tr("已加进你的 PATH：新打开的终端里就能用 ws 了。")
                       : tr("已从你的 PATH 里去掉。已经开着的终端要重开才生效。"));
    else if (error == ERROR_NOT_FOUND)
        setMessage(tr("这一项不是 Win顺 加的，没有动它。"));
    else
        setMessage((add ? tr("没能写进你的 PATH（错误 %1）。") : tr("没能改你的 PATH（错误 %1）。")).arg(error));
    refresh();
}

void CliSetup::setMessage(const QString& message)
{
    if (m_message == message)
        return;
    m_message = message;
    emit messageChanged();
}

void CliSetup::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

} // namespace ws
