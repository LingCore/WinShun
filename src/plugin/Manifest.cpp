#include "Manifest.h"

#include "WebShortcut.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws::plugin {

QString Text::pick(const QString& locale) const
{
    if (locale.startsWith(u"en"_s))
        return en.isEmpty() ? zh : en;
    return zh.isEmpty() ? en : zh;
}

namespace {

struct Named {
    Permission permission;
    const char16_t* name;
};
constexpr Named kPermissions[] = {
    {Permission::FilesWrite, u"files:write"},
    {Permission::Processes, u"processes"},
    {Permission::ClipboardRead, u"clipboard:read"},
    {Permission::ClipboardWrite, u"clipboard:write"},
    {Permission::Ui, u"ui"},
    {Permission::Network, u"network"},
    {Permission::WinshunSearch, u"winshun:search"},
    {Permission::WinshunOpen, u"winshun:open"},
    {Permission::WinshunHistory, u"winshun:history"},
    {Permission::WinshunContent, u"winshun:content"},
    {Permission::FilesRead, u"files:read"},
    {Permission::Notifications, u"notifications"},
    {Permission::NetworkListen, u"network:listen"},
};

// Collects the first error; every check after it is skipped.
struct Check {
    QString error;
    bool failed() const { return !error.isEmpty(); }
    void fail(const QString& text)
    {
        if (error.isEmpty())
            error = text;
    }
};

std::optional<Text> readText(const QJsonValue& value, const QString& where, Check& check)
{
    if (value.isUndefined() || value.isNull())
        return std::nullopt;
    if (value.isString())
        return Text {value.toString(), value.toString()};
    if (value.isObject()) {
        const QJsonObject o = value.toObject();
        Text text {o.value(u"zh"_s).toString(), o.value(u"en"_s).toString()};
        if (text.isEmpty()) {
            check.fail(where + u" 要有 zh 或 en"_s);
            return std::nullopt;
        }
        return text;
    }
    check.fail(where + u" 要是字符串或 {zh, en}"_s);
    return std::nullopt;
}

Text textOr(const QJsonValue& value, const QString& where, Check& check)
{
    return readText(value, where, check).value_or(Text {});
}

QString stringField(const QJsonObject& o, const QString& key, const QString& where, Check& check)
{
    const QJsonValue v = o.value(key);
    if (v.isUndefined() || v.isNull())
        return {};
    if (!v.isString()) {
        check.fail(where + u'.' + key + u" 要是字符串"_s);
        return {};
    }
    return v.toString();
}

int intField(const QJsonObject& o, const QString& key, int fallback, int min, int max, const QString& where, Check& check)
{
    const QJsonValue v = o.value(key);
    if (v.isUndefined() || v.isNull())
        return fallback;
    if (!v.isDouble()) {
        check.fail(where + u'.' + key + u" 要是数字"_s);
        return fallback;
    }
    return std::clamp(static_cast<int>(v.toDouble()), min, max);
}

QStringList stringList(const QJsonValue& v, const QString& where, Check& check)
{
    QStringList out;
    if (v.isString()) {
        out.push_back(v.toString());
        return out;
    }
    if (!v.isArray()) {
        check.fail(where + u" 要是字符串或字符串数组"_s);
        return out;
    }
    for (const QJsonValue& item : v.toArray()) {
        if (!item.isString()) {
            check.fail(where + u" 里有不是字符串的项"_s);
            return out;
        }
        out.push_back(item.toString());
    }
    return out;
}

bool relativeInside(const QString& path, const QString& where, Check& check)
{
    if (!insideFolder(path)) {
        check.fail(where + u" 必须是插件文件夹里的相对路径"_s);
        return false;
    }
    return true;
}

std::optional<Runtime> readRuntime(const QJsonValue& value, Check& check)
{
    if (value.isUndefined() || value.isNull())
        return std::nullopt;
    if (!value.isObject()) {
        check.fail(u"runtime 要是对象"_s);
        return std::nullopt;
    }
    const QJsonObject o = value.toObject();
    const QString where = u"runtime"_s;
    Runtime r;
    const QString mode = stringField(o, u"mode"_s, where, check);
    if (mode == u"once")
        r.mode = RunMode::Once;
    else if (mode == u"resident")
        r.mode = RunMode::Resident;
    else
        check.fail(u"runtime.mode 要是 once 或 resident"_s);
    const QString program = stringField(o, u"program"_s, where, check);
    if (program == u"python")
        r.program = Program::Python;
    else if (program == u"pwsh")
        r.program = Program::Pwsh;
    else if (program == u"powershell")
        r.program = Program::PowerShell;
    else if (program == u"node")
        r.program = Program::Node;
    else if (program.isEmpty())
        check.fail(u"runtime.program 不能为空"_s);
    else {
        r.program = Program::Exe;
        r.programPath = program;
        relativeInside(program, u"runtime.program"_s, check);
    }
    r.args = stringList(o.contains(u"args"_s) ? o.value(u"args"_s) : QJsonValue(QJsonArray()), u"runtime.args"_s, check);
    r.script = stringField(o, u"script"_s, where, check);
    if (r.script.isEmpty()) {
        switch (r.program) {
        case Program::Python: r.script = u"main.py"_s; break;
        case Program::Pwsh:
        case Program::PowerShell: r.script = u"main.ps1"_s; break;
        case Program::Node: r.script = u"main.js"_s; break;
        case Program::Exe: break;
        }
    } else
        relativeInside(r.script, u"runtime.script"_s, check);
    r.cwd = stringField(o, u"cwd"_s, where, check);
    if (!r.cwd.isEmpty())
        relativeInside(r.cwd, u"runtime.cwd"_s, check);
    const QJsonValue env = o.value(u"env"_s);
    if (env.isObject()) {
        const QJsonObject e = env.toObject();
        for (auto it = e.begin(); it != e.end(); ++it) {
            if (it.key().startsWith(u"WS_"_s, Qt::CaseInsensitive)) {
                check.fail(u"runtime.env 不能设置 WS_* 变量"_s);
                break;
            }
            if (!it.value().isString()) {
                check.fail(u"runtime.env 的值要是字符串"_s);
                break;
            }
            r.env.push_back({it.key(), it.value().toString()});
        }
    } else if (!env.isUndefined() && !env.isNull())
        check.fail(u"runtime.env 要是对象"_s);
    const QString encoding = stringField(o, u"encoding"_s, where, check);
    if (encoding.isEmpty() || encoding.compare(u"utf-8"_s, Qt::CaseInsensitive) == 0)
        r.encoding = Encoding::Utf8;
    else if (encoding.compare(u"oem"_s, Qt::CaseInsensitive) == 0)
        r.encoding = Encoding::Oem;
    else
        check.fail(u"runtime.encoding 要是 utf-8 或 oem"_s);
    r.timeoutMs = intField(o, u"timeoutMs"_s, r.mode == RunMode::Resident ? 1500 : 3000, 100, 10000, where, check);
    r.debounceMs = intField(o, u"debounceMs"_s, r.mode == RunMode::Resident ? 0 : 120, 0, 2000, where, check);
    r.idleExitSec = intField(o, u"idleExitSec"_s, 600, 0, 86400, where, check);
    const QString start = stringField(o, u"start"_s, where, check);
    if (start.isEmpty() || start == u"lazy")
        r.start = StartWhen::Lazy;
    else if (start == u"eager")
        r.start = StartWhen::Eager;
    else
        check.fail(u"runtime.start 要是 lazy 或 eager"_s);
    r.memoryMB = intField(o, u"memoryMB"_s, 512, 64, 2048, where, check);
    const QString sandbox = stringField(o, u"sandbox"_s, where, check);
    if (sandbox.isEmpty() || sandbox == u"normal")
        r.sandbox = SandboxLevel::Normal;
    else if (sandbox == u"strict")
        r.sandbox = SandboxLevel::Strict;
    else
        check.fail(u"runtime.sandbox 要是 normal 或 strict"_s);
    return r;
}

Permissions readPermissions(const QJsonValue& value, Check& check)
{
    Permissions p;
    if (value.isUndefined() || value.isNull())
        return p;
    if (!value.isArray()) {
        check.fail(u"permissions 要是数组"_s);
        return p;
    }
    for (const QJsonValue& item : value.toArray()) {
        if (item.isString()) {
            const auto permission = permissionFromName(item.toString());
            if (!permission) {
                check.fail(u"permissions 里的 %1 不认识（可能需要更新的 Win顺）"_s.arg(item.toString()));
                return p;
            }
            p.flags |= static_cast<std::uint32_t>(*permission);
        } else if (item.isObject()) {
            // {"files:read": ["path", …]}: a permission with what it covers.
            const QJsonObject o = item.toObject();
            for (auto it = o.begin(); it != o.end(); ++it) {
                const auto permission = permissionFromName(it.key());
                if (!permission) {
                    check.fail(u"permissions 里的 %1 不认识（可能需要更新的 Win顺）"_s.arg(it.key()));
                    return p;
                }
                p.flags |= static_cast<std::uint32_t>(*permission);
                if (*permission == Permission::FilesRead)
                    p.readPaths += stringList(it.value(), u"permissions.files:read"_s, check);
                else if (*permission == Permission::NetworkListen)
                    p.listens += stringList(it.value(), u"permissions.network:listen"_s, check);
            }
        } else {
            check.fail(u"permissions 里有不是字符串的项"_s);
            return p;
        }
    }
    return p;
}

Trigger readTrigger(const QJsonValue& value, const QString& where, Check& check)
{
    Trigger t;
    if (!value.isObject()) {
        check.fail(where + u" 要是对象"_s);
        return t;
    }
    const QJsonObject o = value.toObject();
    if (o.contains(u"regex"_s)) {
        t.kind = Trigger::Kind::Regex;
        t.regex = stringField(o, u"regex"_s, where, check);
        if (!QRegularExpression(t.regex).isValid())
            check.fail(where + u".regex 不是有效的正则表达式"_s);
        t.minLength = intField(o, u"minLength"_s, 1, 0, 4096, where, check);
        t.maxLength = intField(o, u"maxLength"_s, 64, 1, 4096, where, check);
    } else if (o.contains(u"push"_s)) {
        t.kind = Trigger::Kind::Push;
        const QJsonObject push = o.value(u"push"_s).toObject();
        t.minLength = intField(push, u"minLength"_s, 2, 1, 4096, where + u".push"_s, check);
        t.maxLength = intField(push, u"maxLength"_s, 64, 1, 4096, where + u".push"_s, check);
        t.maxItems = intField(push, u"maxItems"_s, 3, 1, 3, where + u".push"_s, check);
    } else if (o.contains(u"files"_s)) {
        t.kind = Trigger::Kind::Files;
        const QJsonObject files = o.value(u"files"_s).toObject();
        t.fileKind = stringField(files, u"kind"_s, where + u".files"_s, check);
        if (t.fileKind.isEmpty())
            t.fileKind = u"any"_s;
        if (t.fileKind != u"file" && t.fileKind != u"folder" && t.fileKind != u"any")
            check.fail(where + u".files.kind 要是 file、folder 或 any"_s);
        if (files.contains(u"extensions"_s))
            t.extensions = stringList(files.value(u"extensions"_s), where + u".files.extensions"_s, check);
        for (QString& e : t.extensions)
            e = e.toLower().remove(u'.');
        t.maxFiles = intField(files, u"max"_s, 20, 1, 1000, where + u".files"_s, check);
        t.title = readText(files.value(u"title"_s), where + u".files.title"_s, check).value_or(Text {});
    } else if (o.contains(u"clipboard"_s)) {
        t.kind = Trigger::Kind::Clipboard;
        const QJsonObject clip = o.value(u"clipboard"_s).toObject();
        t.regex = stringField(clip, u"regex"_s, where + u".clipboard"_s, check);
        if (!t.regex.isEmpty() && !QRegularExpression(t.regex).isValid())
            check.fail(where + u".clipboard.regex 不是有效的正则表达式"_s);
    } else
        check.fail(where + u" 要是 regex、push、files 或 clipboard 之一"_s);
    return t;
}

Run readRun(const QJsonValue& value, const QString& where, Check& check)
{
    Run run;
    if (value.isUndefined() || value.isNull())
        return run;
    if (!value.isObject()) {
        check.fail(where + u" 要是对象"_s);
        return run;
    }
    const QJsonObject o = value.toObject();
    int kinds = 0;
    if (o.contains(u"url"_s)) {
        ++kinds;
        run.kind = Run::Kind::Url;
        run.url = normalizeWebUrl(stringField(o, u"url"_s, where, check));
        run.home = stringField(o, u"home"_s, where, check);
        if (run.url.isEmpty())
            check.fail(where + u".url 不是网址"_s);
    }
    if (o.contains(u"program"_s)) {
        ++kinds;
        run.kind = Run::Kind::Program;
        run.program = stringField(o, u"program"_s, where, check);
        run.args = stringField(o, u"args"_s, where, check);
        run.cwd = stringField(o, u"cwd"_s, where, check);
        if (run.program.isEmpty())
            check.fail(where + u".program 不能为空"_s);
    }
    if (o.contains(u"folder"_s)) {
        ++kinds;
        run.kind = Run::Kind::Folder;
        run.folder = stringField(o, u"folder"_s, where, check);
        if (run.folder.isEmpty())
            check.fail(where + u".folder 不能为空"_s);
    }
    if (o.contains(u"search"_s)) {
        ++kinds;
        run.kind = Run::Kind::Search;
        const QJsonValue search = o.value(u"search"_s);
        if (search.isString())
            run.query = search.toString();
        else if (search.isObject()) {
            run.query = stringField(search.toObject(), u"query"_s, where + u".search"_s, check);
            run.scope = stringField(search.toObject(), u"scope"_s, where + u".search"_s, check);
        } else
            check.fail(where + u".search 要是字符串或 {query, scope}"_s);
        if (run.scope.isEmpty())
            run.scope = u"all"_s;
        if (run.scope != u"all" && run.scope != u"files" && run.scope != u"folders" && run.scope != u"content")
            check.fail(where + u".search.scope 要是 all、files、folders 或 content"_s);
    }
    if (o.contains(u"items"_s)) {
        ++kinds;
        run.kind = Run::Kind::Items;
        if (!o.value(u"items"_s).isArray())
            check.fail(where + u".items 要是数组"_s);
        run.items = o.value(u"items"_s).toArray();
        for (const QJsonValue& item : run.items) {
            if (!item.isObject() || !item.toObject().value(u"title"_s).isString()) {
                check.fail(where + u".items 里的每一项要有 title"_s);
                break;
            }
        }
    }
    if (kinds == 0)
        check.fail(where + u" 要是 url、program、folder、search 或 items 之一"_s);
    else if (kinds > 1)
        check.fail(where + u" 只能是 url、program、folder、search、items 中的一种"_s);
    return run;
}

Command readCommand(const QJsonValue& value, int index, bool hasRuntime, Check& check)
{
    Command c;
    const QString where = u"commands[%1]"_s.arg(index);
    if (!value.isObject()) {
        check.fail(where + u" 要是对象"_s);
        return c;
    }
    const QJsonObject o = value.toObject();
    c.id = stringField(o, u"id"_s, where, check);
    if (c.id.isEmpty())
        check.fail(where + u".id 不能为空"_s);
    else if (!validPluginId(c.id))
        check.fail(where + u".id 只能用小写字母、数字、点、连字符和下划线"_s);
    c.title = textOr(o.value(u"title"_s), where + u".title"_s, check);
    c.description = textOr(o.value(u"description"_s), where + u".description"_s, check);
    c.hint = textOr(o.value(u"hint"_s), where + u".hint"_s, check);
    c.icon = stringField(o, u"icon"_s, where, check);
    if (c.icon.endsWith(u".svg"_s, Qt::CaseInsensitive))
        check.fail(where + u".icon 是 SVG：Win顺 不带 SVG 支持，请用 PNG"_s);
    if (o.contains(u"keyword"_s)) {
        c.keywords = stringList(o.value(u"keyword"_s), where + u".keyword"_s, check);
        for (const QString& keyword : c.keywords) {
            if (keyword.contains(u' '))
                check.fail(where + u".keyword 含空格"_s);
            else if (!isValidWebKeyword(keyword))
                check.fail(where + u".keyword 不能为空，也不能含引号"_s);
        }
    }
    if (o.contains(u"triggers"_s)) {
        const QJsonValue triggers = o.value(u"triggers"_s);
        if (!triggers.isArray())
            check.fail(where + u".triggers 要是数组"_s);
        int i = 0;
        for (const QJsonValue& t : triggers.toArray())
            c.triggers.push_back(readTrigger(t, where + u".triggers[%1]"_s.arg(i++), check));
    }
    const QString mode = stringField(o, u"mode"_s, where, check);
    if (mode.isEmpty() || mode == u"list")
        c.mode = Command::Mode::List;
    else if (mode == u"instant")
        c.mode = Command::Mode::Instant;
    else
        check.fail(where + u".mode 要是 list 或 instant"_s);
    if (o.contains(u"actions"_s)) {
        const QJsonValue actions = o.value(u"actions"_s);
        if (!actions.isArray())
            check.fail(where + u".actions 要是数组"_s);
        int i = 0;
        for (const QJsonValue& a : actions.toArray()) {
            const QString aw = where + u".actions[%1]"_s.arg(i++);
            if (!a.isObject()) {
                check.fail(aw + u" 要是对象"_s);
                break;
            }
            Action action;
            action.id = stringField(a.toObject(), u"id"_s, aw, check);
            action.title = textOr(a.toObject().value(u"title"_s), aw + u".title"_s, check);
            action.key = stringField(a.toObject(), u"key"_s, aw, check);
            if (action.id.isEmpty())
                check.fail(aw + u".id 不能为空"_s);
            c.actions.push_back(action);
        }
    }
    c.run = readRun(o.value(u"run"_s), where + u".run"_s, check);
    if (c.run.kind == Run::Kind::None && !hasRuntime)
        check.fail(where + u" 没有 run，插件也没有 runtime：不知道怎么执行"_s);
    if (o.contains(u"cache"_s)) {
        const QJsonValue cache = o.value(u"cache"_s);
        if (cache.isObject())
            c.cacheSeconds = intField(cache.toObject(), u"seconds"_s, 0, 0, 86400, where + u".cache"_s, check);
        else if (cache.isDouble())
            c.cacheSeconds = std::clamp(static_cast<int>(cache.toDouble()), 0, 86400);
        else
            check.fail(where + u".cache 要是 {seconds} 或数字"_s);
    }
    if (o.value(u"ai"_s).isObject())
        c.ai = o.value(u"ai"_s).toObject();
    return c;
}

Setting readSetting(const QJsonValue& value, int index, Check& check)
{
    Setting s;
    const QString where = u"settings[%1]"_s.arg(index);
    if (!value.isObject()) {
        check.fail(where + u" 要是对象"_s);
        return s;
    }
    const QJsonObject o = value.toObject();
    s.key = stringField(o, u"key"_s, where, check);
    if (s.key.isEmpty())
        check.fail(where + u".key 不能为空"_s);
    s.type = stringField(o, u"type"_s, where, check);
    static const QStringList kTypes = {u"text"_s, u"password"_s, u"bool"_s, u"number"_s, u"choice"_s, u"folder"_s, u"file"_s, u"hotkey"_s};
    if (!kTypes.contains(s.type))
        check.fail(where + u".type 要是 text、password、bool、number、choice、folder、file 或 hotkey"_s);
    s.title = textOr(o.value(u"title"_s), where + u".title"_s, check);
    s.hint = textOr(o.value(u"hint"_s), where + u".hint"_s, check);
    s.defaultValue = o.value(u"default"_s);
    if (o.contains(u"options"_s)) {
        if (!o.value(u"options"_s).isArray())
            check.fail(where + u".options 要是数组"_s);
        s.options = o.value(u"options"_s).toArray();
    }
    return s;
}

} // namespace

std::optional<Permission> permissionFromName(QStringView name)
{
    for (const Named& n : kPermissions) {
        if (name == n.name)
            return n.permission;
    }
    return std::nullopt;
}

QString permissionName(Permission p)
{
    for (const Named& n : kPermissions) {
        if (n.permission == p)
            return QString(n.name);
    }
    return {};
}

QStringList Permissions::names() const
{
    QStringList out;
    for (const Named& n : kPermissions) {
        if (has(n.permission))
            out.push_back(QString(n.name));
    }
    return out;
}

const Command* Manifest::command(QStringView commandId) const
{
    for (const Command& c : commands) {
        if (c.id == commandId)
            return &c;
    }
    return nullptr;
}

std::optional<Manifest> Manifest::parse(const QByteArray& json, QString* error)
{
    Check check;
    const auto fail = [&](const QString& text) {
        if (error)
            *error = text;
        return std::nullopt;
    };
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return fail(u"plugin.json 不是有效的 JSON：%1（第 %2 个字符）"_s.arg(parseError.errorString()).arg(parseError.offset));
    if (!document.isObject())
        return fail(u"plugin.json 要是一个 JSON 对象"_s);
    const QJsonObject o = document.object();

    Manifest m;
    const QJsonValue manifestVersion = o.value(u"manifest"_s);
    if (!manifestVersion.isDouble())
        return fail(u"缺少 manifest（架构版本，现在是 1）"_s);
    m.manifestVersion = static_cast<int>(manifestVersion.toDouble());
    if (m.manifestVersion > kManifestVersion)
        return fail(u"这个插件要 manifest %1，这个 Win顺 只认识到 %2：请升级 Win顺"_s.arg(m.manifestVersion).arg(kManifestVersion));
    if (m.manifestVersion < 1)
        return fail(u"manifest 要是 1"_s);

    m.id = stringField(o, u"id"_s, u"顶层"_s, check);
    if (m.id.isEmpty())
        check.fail(u"缺少 id（作者.名字）"_s);
    else if (!validPluginId(m.id))
        check.fail(u"id 只能用小写字母、数字、点、连字符和下划线（作者.名字）"_s);
    const auto name = readText(o.value(u"name"_s), u"name"_s, check);
    if (!name && !check.failed())
        check.fail(u"缺少 name"_s);
    m.name = name.value_or(Text {});
    m.version = stringField(o, u"version"_s, u"顶层"_s, check);
    static const QRegularExpression kVersion(u"^\\d+(\\.\\d+)*([-+][0-9A-Za-z.-]+)?$"_s);
    if (m.version.isEmpty())
        check.fail(u"缺少 version（如 1.0.0）"_s);
    else if (!kVersion.match(m.version).hasMatch())
        check.fail(u"version 要像 1.2.0 这样"_s);
    m.description = textOr(o.value(u"description"_s), u"description"_s, check);
    m.author = stringField(o, u"author"_s, u"顶层"_s, check);
    m.homepage = stringField(o, u"homepage"_s, u"顶层"_s, check);
    m.license = stringField(o, u"license"_s, u"顶层"_s, check);
    m.icon = stringField(o, u"icon"_s, u"顶层"_s, check);
    if (m.icon.endsWith(u".svg"_s, Qt::CaseInsensitive))
        check.fail(u"icon 是 SVG：Win顺 不带 SVG 支持，请用 PNG"_s);
    else if (!m.icon.isEmpty())
        relativeInside(m.icon, u"icon"_s, check);
    m.minWinShun = stringField(o, u"minWinShun"_s, u"顶层"_s, check);
    if (!m.minWinShun.isEmpty() && !kVersion.match(m.minWinShun).hasMatch())
        check.fail(u"minWinShun 要像 0.7.0 这样"_s);
    m.runtime = readRuntime(o.value(u"runtime"_s), check);
    m.permissions = readPermissions(o.value(u"permissions"_s), check);

    const QJsonValue commands = o.value(u"commands"_s);
    if (!commands.isArray() || commands.toArray().isEmpty())
        check.fail(u"commands 要至少有一个命令"_s);
    else {
        int i = 0;
        for (const QJsonValue& c : commands.toArray())
            m.commands.push_back(readCommand(c, i++, m.runtime.has_value(), check));
        for (qsizetype a = 0; a < m.commands.size() && !check.failed(); ++a) {
            for (qsizetype b = a + 1; b < m.commands.size(); ++b) {
                if (m.commands[a].id == m.commands[b].id) {
                    check.fail(u"commands 里有两个 id 都是 %1 的命令"_s.arg(m.commands[a].id));
                    break;
                }
            }
        }
    }
    if (o.contains(u"settings"_s)) {
        const QJsonValue settings = o.value(u"settings"_s);
        if (!settings.isArray())
            check.fail(u"settings 要是数组"_s);
        int i = 0;
        for (const QJsonValue& s : settings.toArray())
            m.settings.push_back(readSetting(s, i++, check));
    }
    if (o.value(u"ai"_s).isObject())
        m.aiExpose = o.value(u"ai"_s).toObject().value(u"expose"_s).toBool();

    if (check.failed())
        return fail(check.error);
    return m;
}

std::optional<Manifest> Manifest::load(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = u"读不了 %1：%2"_s.arg(path, file.errorString());
        return std::nullopt;
    }
    return parse(file.readAll(), error);
}

int compareVersions(QStringView a, QStringView b)
{
    const auto split = [](QStringView v, QList<int>& numbers, QStringView& suffix) {
        qsizetype end = v.size();
        for (qsizetype i = 0; i < v.size(); ++i) {
            if (v[i] == u'-' || v[i] == u'+') {
                end = i;
                break;
            }
        }
        suffix = v.mid(end);
        for (const QStringView part : v.left(end).split(u'.'))
            numbers.push_back(part.toInt());
    };
    QList<int> na, nb;
    QStringView sa, sb;
    split(a, na, sa);
    split(b, nb, sb);
    for (qsizetype i = 0; i < std::max(na.size(), nb.size()); ++i) {
        const int x = i < na.size() ? na[i] : 0;
        const int y = i < nb.size() ? nb[i] : 0;
        if (x != y)
            return x < y ? -1 : 1;
    }
    // A pre-release ranks below the release it precedes ("1.0.0-beta" < "1.0.0").
    if (sa.isEmpty() != sb.isEmpty())
        return sa.isEmpty() ? 1 : -1;
    return sa.compare(sb);
}

bool validPluginId(QStringView id) noexcept
{
    if (id.isEmpty() || id.size() > 64 || id.front() == u'.' || id.back() == u'.')
        return false;
    for (const QChar c : id) {
        const char16_t u = c.unicode();
        if (!((u >= u'a' && u <= u'z') || (u >= u'0' && u <= u'9') || u == u'.' || u == u'_' || u == u'-'))
            return false;
    }
    return true;
}

bool insideFolder(QStringView relative) noexcept
{
    if (relative.isEmpty() || relative.startsWith(u'/') || relative.startsWith(u'\\') || relative.contains(u':'))
        return false;
    for (const QStringView part : relative.split(u'/')) {
        for (const QStringView piece : part.split(u'\\')) {
            if (piece == u"..")
                return false;
        }
    }
    return true;
}

} // namespace ws::plugin
