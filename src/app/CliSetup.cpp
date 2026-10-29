#include "CliSetup.h"

#include "SettingsEditor.h"
#include "platform/Integrity.h"
#include "platform/SettingBroadcast.h"
#include "platform/Shell.h"

#include <QCoreApplication>
#include <QPointer>

#include <thread>

using namespace Qt::StringLiterals;

namespace ws {

CliSetup::CliSetup(SettingsEditor* editor, QObject* parent)
    : QObject(parent)
    , m_editor(editor)
{
    // Before the first look at the PATH is back (it is made off the GUI thread), what
    // is known at once: where ws.exe should be, and whether it is. The PATH is not.
    cli::Status first;
    first.bin = cli::binFolder();
    first.exe = cli::wsExe(first.bin);
    m_state = cli::toVariant(first);
    m_state.insert(u"checked"_s, false);
}

QString CliSetup::example() const
{
    return tr("ws 报告");
}

void CliSetup::refresh()
{
    if (m_neverLooks)
        return;
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
    // After a change the buttons stay off until what the PATH now holds is in the state
    // (the look made for it; one made earlier, by the window coming to the front, says
    // nothing yet): "从 PATH 移除" clicked again in between said the entry was not WinShun's.
    if (m_busy && m_settleLook != 0 && look >= m_settleLook)
        setBusy(false);
}

// The PATH is the account's whose registry this program writes (HKCU). Started as another
// account's administrator, that is not the user at the desktop, whose terminals and Claude
// Code would never see the folder (and the Skill goes to the user's, ws running as them).
bool CliSetup::refusesOtherAccount()
{
    if (m_probe || !integrity::processElevated() || shell::runsAsShellUser())
        return false;
    setMessage(tr("Win顺 现在是用另一个账户（管理员）运行的，PATH 是那个账户的，不是你登录的这个。请先退出 Win顺，再正常打开它，然后在这里启用。"));
    return true;
}

void CliSetup::enable()
{
    if (m_busy || refusesOtherAccount())
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
    if (!m_busy && !refusesOtherAccount())
        changePath(false);
}

void CliSetup::changePath(bool add)
{
    m_settleLook = 0;
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
    // Still busy: apply() ends it when the look made now is in.
    m_settleLook = m_looks + 1;
    if (error == 0)
        setMessage(add ? tr("命令行已启用：新打开的终端里就能用 ws 了。")
                       : tr("已从你的 PATH 里去掉。已经开着的终端要重开才生效。"));
    else if (error == ERROR_NOT_FOUND)
        setMessage(tr("这一项不是 Win顺 加的，没有动它。"));
    else if (error == ERROR_UNSUPPORTED_TYPE)
        setMessage(tr("你的 PATH 不是文本类型的值（注册表里的类型不对），为了不弄坏它，Win顺 没有动。可以用 ws 的完整路径，或自己修好它。"));
    else if (error == ERROR_BUSY || error == ERROR_MORE_DATA)
        setMessage(tr("你的 PATH 刚被别的程序改着，没有写成；再点一次。"));
    else if (error == ERROR_ACCESS_DENIED)
        setMessage(tr("没有权限改你的 PATH（错误 5）。"));
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
