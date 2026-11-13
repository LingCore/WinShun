#include "DialogRuleStore.h"

#include "platform/FileDialog.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws {

namespace {

constexpr int kLearnSeconds = 5;

// What a new file starts with: a rule that names no real program, to copy from.
QByteArray starter()
{
    dialogrules::Rule example;
    example.name = L"示例：把 program 改成程序名，这条规则才会生效";
    example.program = L"example.exe";
    example.windowClass = L"#32770";
    example.title = L"添加数据";
    example.box.type = L"edit";
    example.box.name = L"名称:";
    return QJsonDocument(QJsonObject {{u"dialogs"_s, QJsonArray {dialogrules::toJson(example)}}}).toJson();
}

bool write(const QString& path, const QByteArray& json)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(json) == json.size() && file.commit();
}

bool sameWindow(const QJsonObject& a, const QJsonObject& b)
{
    return a.value("program"_L1).toString().compare(b.value("program"_L1).toString(), Qt::CaseInsensitive) == 0
        && a.value("class"_L1) == b.value("class"_L1) && a.value("title"_L1) == b.value("title"_L1);
}

} // namespace

DialogRuleStore::DialogRuleStore(const QString& folder, QObject* parent)
    : QObject(parent)
    , m_folder(folder)
{
    m_reload.setSingleShot(true);
    m_reload.setInterval(300ms);
    connect(&m_reload, &QTimer::timeout, this, &DialogRuleStore::reload);
    // The folder too: the file may not be there yet, and editors that save by
    // replacing the file drop a watch on the file itself.
    m_watcher.addPath(m_folder);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_reload.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_reload.start(); });
    m_countdown.setInterval(1s);
    connect(&m_countdown, &QTimer::timeout, this, [this] {
        if (--m_left > 0) {
            emit status(tr("%1 秒内切到那个窗口，光标点进填文件名的输入框…").arg(m_left), false);
            return;
        }
        m_countdown.stop();
        look();
    });
    reload();
}

QString DialogRuleStore::filePath() const
{
    return QDir(m_folder).filePath(u"dialogs.json"_s);
}

void DialogRuleStore::reload()
{
    const QString path = filePath();
    // The folder changes all the time (history, the clipboard): only a file
    // that is another one now is read.
    const QFileInfo info(path);
    const std::pair stamp(info.lastModified(), info.exists() ? info.size() : -1);
    if (stamp == m_stamp)
        return;
    m_stamp = stamp;
    QFile file(path);
    QStringList problems;
    std::vector<dialogrules::Rule> rules;
    if (file.open(QIODevice::ReadOnly))
        rules = dialogrules::parse(file.readAll(), &problems);
    for (const QString& problem : std::as_const(problems))
        qWarning().noquote() << problem;
    dialogrules::setRules(std::move(rules));
    emit changed();
    if (QFileInfo::exists(path) && !m_watcher.files().contains(path))
        m_watcher.addPath(path);
}

QString DialogRuleStore::ensureFile()
{
    const QString path = filePath();
    return QFileInfo::exists(path) || write(path, starter()) ? path : QString();
}

void DialogRuleStore::learn()
{
    if (learning())
        return;
    m_left = kLearnSeconds;
    emit status(tr("%1 秒内切到那个窗口，光标点进填文件名的输入框…").arg(m_left), false);
    m_countdown.start();
}

void DialogRuleStore::look()
{
    HWND window = ::GetForegroundWindow();
    if (const HWND root = window ? ::GetAncestor(window, GA_ROOT) : nullptr)
        window = root;
    DWORD process = 0;
    if (!window || !::GetWindowThreadProcessId(window, &process) || process == ::GetCurrentProcessId()) {
        emit status(tr("没有切到别的程序的窗口，再点一次试试"), true);
        return;
    }
    if (filedialog::kind(window) != filedialog::Kind::None && !filedialog::byRule(window)) {
        emit status(tr("这个窗口 Win顺 本来就认识，不用加规则"), true);
        return;
    }
    if (!m_worker)
        m_worker = std::make_unique<ComWorker>();
    m_looking = true;
    m_worker->post([this, window, path = filePath()] {
        const dialogrules::Seen seen = dialogrules::describe(window);
        QString text;
        if (seen.rule.program.empty())
            text = tr("这个窗口里没找到能填路径的输入框（它可能是管理员权限的程序，或不支持辅助功能）");
        else if (!add(path, seen))
            text = tr("规则文件写不进去：%1").arg(QDir::toNativeSeparators(path));
        else
            text = tr("已记下“%1”（%2），现在就能用。认错了输入框，点“规则文件”改")
                       .arg(QString::fromStdWString(seen.rule.name), QString::fromStdWString(seen.rule.program));
        QMetaObject::invokeMethod(this, [this, text] {
            m_looking = false;
            reload();
            emit status(text, true);
        }, Qt::QueuedConnection);
    });
}

bool DialogRuleStore::add(const QString& path, const dialogrules::Seen& seen)
{
    QJsonObject root;
    if (QFile file(path); file.open(QIODevice::ReadOnly)) {
        QJsonParseError error {};
        const QByteArray text = file.readAll();
        root = QJsonDocument::fromJson(text, &error).object();
        // A file the user is in the middle of editing is not written over.
        if (error.error != QJsonParseError::NoError && !text.trimmed().isEmpty())
            return false;
    }
    QJsonObject entry = dialogrules::toJson(seen.rule);
    // Not read back: what was there, for picking another box by hand.
    entry.insert("seen"_L1, QJsonObject {{u"boxes"_s, QJsonArray::fromStringList(seen.boxes)},
                                {u"buttons"_s, QJsonArray::fromStringList(seen.buttons)}});
    QJsonArray entries = root.value("dialogs"_L1).toArray();
    for (qsizetype i = entries.size() - 1; i >= 0; --i) {
        if (sameWindow(entries.at(i).toObject(), entry))
            entries.removeAt(i);
    }
    entries.append(entry);
    root.insert("dialogs"_L1, entries);
    return write(path, QJsonDocument(root).toJson());
}

} // namespace ws
