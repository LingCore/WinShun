#include "McpClients.h"

#include "platform/Shell.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QSaveFile>
#include <QVariantMap>

#include <algorithm>
#include <thread>

using namespace Qt::StringLiterals;

namespace ws::mcpclients {

namespace {

// The texts shown for the connections, in a context of their own (winshun_en.ts).
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(McpClients)
};

const QLatin1StringView kServerKey("winshun");

QString env(const char* name)
{
    return QDir::fromNativeSeparators(qEnvironmentVariable(name));
}

QString home()
{
    return env("USERPROFILE");
}

QString claudeDesktopConfig()
{
    return env("APPDATA") + u"/Claude/claude_desktop_config.json"_s;
}

QString cursorConfig()
{
    return home() + u"/.cursor/mcp.json"_s;
}

QString vsCodeConfig()
{
    return env("APPDATA") + u"/Code/User/mcp.json"_s;
}

QString claudeCodeConfig()
{
    return home() + u"/.claude.json"_s;
}

// The relay's entry as the file wants it.
QJsonObject serverEntry(const QString& id)
{
    QJsonObject entry;
    if (id == u"vscode")
        entry.insert(u"type"_s, u"stdio"_s);
    entry.insert(u"command"_s, relayPath());
    // --start-winshun: a tool call starts Win顺 (through its autostart task,
    // no UAC prompt) when it is not running at all.
    entry.insert(u"args"_s, QJsonArray {u"--start-winshun"_s});
    return entry;
}

QString topKey(const QString& id)
{
    return id == u"vscode" ? u"servers"_s : u"mcpServers"_s;
}

// Whether the file names the relay next to this program.
bool namesRelay(const QString& path, const QString& key)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject entry = root.value(key).toObject().value(kServerKey).toObject();
    const QString command = QDir::fromNativeSeparators(entry.value(u"command"_s).toString());
    return !command.isEmpty() && command.compare(QDir::fromNativeSeparators(relayPath()), Qt::CaseInsensitive) == 0;
}

// Puts the relay into the JSON file, keeping everything else in it. The
// file is backed up first; one that is not plain JSON (comments: VS Code
// allows them) is left alone.
Result mergeInto(const QString& path, const QString& key, const QJsonObject& entry, const QString& restart)
{
    QJsonObject root;
    QFile file(path);
    const bool existed = file.exists();
    if (existed) {
        if (!file.open(QIODevice::ReadOnly))
            return {false, Tr::tr("读不了 %1").arg(QDir::toNativeSeparators(path))};
        const QByteArray text = file.readAll();
        file.close();
        if (!text.trimmed().isEmpty()) {
            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(text, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject())
                return {false,
                    Tr::tr("%1 不是标准的 JSON（可能带注释），没有改动它。请“复制配置”后自己加进去。")
                        .arg(QDir::toNativeSeparators(path))};
            root = document.object();
        }
        QFile::remove(path + u".bak"_s);
        QFile::copy(path, path + u".bak"_s);
    }
    QJsonObject servers = root.value(key).toObject();
    servers.insert(kServerKey, entry);
    root.insert(key, servers);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0
        || !out.commit())
        return {false, Tr::tr("写不了 %1").arg(QDir::toNativeSeparators(path))};
    return {true, Tr::tr("已写入 %1。%2").arg(QDir::toNativeSeparators(path), restart)};
}

// The claude command: on PATH, in its own install folder, or the one the
// VS Code extension brings along (the newest).
QString claudeCommand()
{
    for (const QString& dir : env("PATH").split(u';', Qt::SkipEmptyParts)) {
        for (const QString& name : {u"claude.exe"_s, u"claude.cmd"_s}) {
            const QString candidate = QDir(dir).filePath(name);
            if (QFileInfo::exists(candidate))
                return QDir::toNativeSeparators(candidate);
        }
    }
    const QString local = home() + u"/.local/bin/claude.exe"_s;
    if (QFileInfo::exists(local))
        return QDir::toNativeSeparators(local);
    QStringList bundled;
    QDirIterator it(home() + u"/.vscode/extensions"_s, {u"anthropic.claude-code-*"_s}, QDir::Dirs | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        const QString exe = it.next() + u"/resources/native-binary/claude.exe"_s;
        if (QFileInfo::exists(exe))
            bundled.push_back(exe);
    }
    if (bundled.isEmpty())
        return {};
    std::ranges::sort(bundled);
    return QDir::toNativeSeparators(bundled.last());
}

QString quoted(const QString& path)
{
    return u"\""_s + path + u"\""_s;
}

} // namespace

namespace {
// Where the last install put the Skill, as ws printed it (the GUI thread's).
QString g_installedSkill;
} // namespace

QString skillPath()
{
    if (!g_installedSkill.isEmpty())
        return g_installedSkill;
    const QString config = env("CLAUDE_CONFIG_DIR");
    return QDir::toNativeSeparators((config.isEmpty() ? home() + u"/.claude"_s : config) + u"/skills/winshun/SKILL.md"_s);
}

QString relayPath()
{
    const QString path = QCoreApplication::applicationDirPath() + u"/WinShunMcp.exe"_s;
    return QFileInfo::exists(path) ? QDir::toNativeSeparators(path) : QString();
}

std::vector<Client> detect()
{
    std::vector<Client> clients;
    {
        Client c {u"claude-desktop"_s, u"Claude Desktop"_s};
        c.installed = QFileInfo(env("APPDATA") + u"/Claude"_s).isDir() || QFileInfo(env("LOCALAPPDATA") + u"/AnthropicClaude"_s).isDir();
        c.configPath = QDir::toNativeSeparators(claudeDesktopConfig());
        c.connected = namesRelay(claudeDesktopConfig(), u"mcpServers"_s);
        c.note = Tr::tr("完全退出并重新打开 Claude Desktop 后生效");
        clients.push_back(c);
    }
    {
        Client c {u"claude-code"_s, u"Claude Code"_s};
        c.installed = !claudeCommand().isEmpty() || QFileInfo(home() + u"/.claude"_s).isDir()
            || (!env("CLAUDE_CONFIG_DIR").isEmpty() && QFileInfo(env("CLAUDE_CONFIG_DIR")).isDir());
        c.configPath = QDir::toNativeSeparators(claudeCodeConfig());
        c.connected = namesRelay(claudeCodeConfig(), u"mcpServers"_s);
        c.note = Tr::tr("加在用户范围（所有项目）；新开的会话里生效");
        c.skillPath = skillPath();
        c.skillInstalled = QFileInfo::exists(c.skillPath);
        clients.push_back(c);
    }
    {
        Client c {u"cursor"_s, u"Cursor"_s};
        c.installed = QFileInfo(home() + u"/.cursor"_s).isDir() || QFileInfo(env("LOCALAPPDATA") + u"/Programs/cursor"_s).isDir();
        c.configPath = QDir::toNativeSeparators(cursorConfig());
        c.connected = namesRelay(cursorConfig(), u"mcpServers"_s);
        c.note = Tr::tr("Cursor 的设置 → MCP 里能看到它，马上可用");
        clients.push_back(c);
    }
    {
        Client c {u"vscode"_s, u"VS Code"_s};
        c.installed = QFileInfo(env("APPDATA") + u"/Code/User"_s).isDir()
            || QFileInfo(env("LOCALAPPDATA") + u"/Programs/Microsoft VS Code"_s).isDir();
        c.configPath = QDir::toNativeSeparators(vsCodeConfig());
        c.connected = namesRelay(vsCodeConfig(), u"servers"_s);
        c.note = Tr::tr("VS Code 的 Copilot 聊天（代理模式）里能用；“MCP: 列出服务器”可以查看");
        clients.push_back(c);
    }
    return clients;
}

QVariantList toVariant(const std::vector<Client>& clients)
{
    QVariantList out;
    for (const Client& c : clients) {
        out.push_back(QVariantMap {{u"id"_s, c.id}, {u"name"_s, c.name}, {u"installed"_s, c.installed},
            {u"connected"_s, c.connected}, {u"configPath"_s, c.configPath}, {u"note"_s, c.note},
            {u"skillInstalled"_s, c.skillInstalled}, {u"skillPath"_s, c.skillPath}});
    }
    return out;
}

void connect(const QString& id, QObject* context, std::function<void(Result)> done)
{
    if (relayPath().isEmpty()) {
        done({false, Tr::tr("没找到 WinShunMcp.exe，重新安装 Win顺 就有了")});
        return;
    }
    if (id == u"claude-desktop") {
        done(mergeInto(claudeDesktopConfig(), u"mcpServers"_s, serverEntry(id), Tr::tr("完全退出并重新打开 Claude Desktop 后生效。")));
    } else if (id == u"cursor") {
        done(mergeInto(cursorConfig(), u"mcpServers"_s, serverEntry(id), Tr::tr("Cursor 会马上认出它（设置 → MCP）。")));
    } else if (id == u"vscode") {
        done(mergeInto(vsCodeConfig(), u"servers"_s, serverEntry(id), Tr::tr("在 VS Code 的聊天里（代理模式）就能用了。")));
    } else if (id == u"claude-code") {
        const QString claude = claudeCommand();
        if (claude.isEmpty()) {
            done({false, Tr::tr("没找到 claude 命令。请“复制命令”，在终端里运行它。")});
            return;
        }
        // Its own command keeps its file in order: remove any old entry (a
        // different path, say), then add this one, in the user scope.
        auto* process = new QProcess(context);
        process->setProgram(claude);
        process->setArguments({u"mcp"_s, u"remove"_s, u"-s"_s, u"user"_s, QString(kServerKey)});
        process->setProcessChannelMode(QProcess::MergedChannels);
        QObject::connect(process, &QProcess::finished, context, [process, claude, done = std::move(done)](int, QProcess::ExitStatus) {
            auto* add = new QProcess(process->parent());
            add->setProgram(claude);
            add->setArguments({u"mcp"_s, u"add"_s, u"--scope"_s, u"user"_s, QString(kServerKey), u"--"_s, relayPath()});
            add->setProcessChannelMode(QProcess::MergedChannels);
            QObject::connect(add, &QProcess::finished, add, [add, done](int code, QProcess::ExitStatus status) {
                const QString output = QString::fromUtf8(add->readAll()).trimmed();
                if (status == QProcess::NormalExit && code == 0)
                    done({true, Tr::tr("已加到 Claude Code（用户范围）。新开的会话里就能用；claude mcp list 可以查看。")});
                else
                    done({false, Tr::tr("claude mcp add 没有成功：%1").arg(output.isEmpty() ? QString::number(code) : output)});
                add->deleteLater();
            });
            QObject::connect(add, &QProcess::errorOccurred, add, [add, done](QProcess::ProcessError) {
                done({false, Tr::tr("运行不了 claude 命令：%1").arg(add->errorString())});
                add->deleteLater();
            });
            add->start();
            process->deleteLater();
        });
        QObject::connect(process, &QProcess::errorOccurred, context, [process, done](QProcess::ProcessError) {
            done({false, Tr::tr("运行不了 claude 命令：%1").arg(process->errorString())});
            process->deleteLater();
        });
        process->start();
    } else {
        done({false, Tr::tr("不认识的客户端")});
    }
}

QString snippet(const QString& id)
{
    const QString relay = relayPath();
    if (id == u"claude-code")
        return u"claude mcp add --scope user winshun -- "_s + quoted(relay) + u" --start-winshun"_s;
    const QJsonObject root {{topKey(id), QJsonObject {{kServerKey, serverEntry(id)}}}};
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void installSkill(const QString& wsExe, QObject* context, std::function<void(Result)> done)
{
    if (wsExe.isEmpty()) {
        done({false, Tr::tr("没找到 ws.exe，重新安装 Win顺 就有了")});
        return;
    }
    constexpr unsigned long kTimeoutMs = 20'000;
    std::thread([context = QPointer<QObject>(context), program = wsExe.toStdWString(), done = std::move(done)] {
        // --encoding utf8 whatever WS_ENCODING says in this program's environment: its words are read as UTF-8 here.
        const shell::Captured run = shell::runUnelevatedCaptured(program, L"skill --install claude --encoding utf8", kTimeoutMs);
        // ws prints the path it wrote and exits 0; anything else, it says why.
        const QString output = QString::fromUtf8(run.output.data(), static_cast<qsizetype>(run.output.size())).trimmed();
        Result result;
        if (!run.started) {
            result.message = Tr::tr("没能运行 ws.exe");
        } else if (run.timedOut) {
            result.message = Tr::tr("ws.exe 在 %1 秒内没有回答，已结束它").arg(kTimeoutMs / 1000);
        } else if (run.exitCode == 0) {
            result.ok = true;
            const QString path = output.section(u'\n', 0, 0).trimmed();
            result.file = path;
            result.message = Tr::tr("已装 Skill：%1。Claude Code 新开的会话里就能用。").arg(path.isEmpty() ? skillPath() : path);
        } else {
            // Its own words, briefly: the first lines, not a screenful.
            QString why = output.left(300);
            if (output.size() > 300)
                why += u"…"_s;
            result.message = Tr::tr("装 Skill 没有成功（退出码 %1）：%2").arg(run.exitCode).arg(why.isEmpty() ? Tr::tr("ws.exe 没有说明原因") : why);
        }
        // On the GUI thread, which asks whether `context` is still there (it may go meanwhile).
        QMetaObject::invokeMethod(QCoreApplication::instance(), [context, done, result] {
            // Whether it is there is judged from where ws put it, not from where this program
            // thinks it goes: ws ran with the user's environment as the registry has it now.
            if (result.ok && !result.file.isEmpty())
                g_installedSkill = QDir::toNativeSeparators(result.file);
            if (context)
                done(result);
        }, Qt::QueuedConnection);
    }).detach();
}

} // namespace ws::mcpclients
