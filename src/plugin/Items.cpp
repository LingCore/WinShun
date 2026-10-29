#include "Items.h"
#include <QDateTime>
#include <cmath>

#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws::plugin {

IconSpec parseIcon(const QString& text)
{
    IconSpec spec;
    if (text.isEmpty())
        return spec;
    struct Prefix {
        const char16_t* prefix;
        IconSpec::Kind kind;
    };
    static constexpr Prefix kPrefixes[] = {
        {u"file:", IconSpec::Kind::File},
        {u"ext:", IconSpec::Kind::Ext},
        {u"app:", IconSpec::Kind::App},
        {u"glyph:", IconSpec::Kind::Glyph},
        {u"color:", IconSpec::Kind::Color},
        {u"data:", IconSpec::Kind::Data},
        {u"site:", IconSpec::Kind::Site},
    };
    for (const Prefix& p : kPrefixes) {
        if (text.startsWith(QStringView(p.prefix), Qt::CaseInsensitive)) {
            const QString value = text.mid(QStringView(p.prefix).size());
            if (value.isEmpty())
                return spec;
            if (p.kind == IconSpec::Kind::Data) {
                if (!value.startsWith(u"image/"_s) || value.size() > 64 * 1024 * 4 / 3 + 64)
                    return spec; // not an image, or more than 64 KB of it
                spec.value = text; // the whole data: URL
            } else if (p.kind == IconSpec::Kind::Site) {
                // Only the site: its icon comes from its home page, and
                // nothing of the address past the host leaves the computer.
                const QUrl url(value, QUrl::TolerantMode);
                const QString scheme = url.scheme().toLower();
                if ((scheme != u"https" && scheme != u"http") || url.host().isEmpty())
                    return spec;
                QUrl home;
                home.setScheme(scheme);
                home.setHost(url.host());
                home.setPort(url.port());
                home.setPath(u"/"_s);
                spec.value = home.toString();
            } else
                spec.value = value;
            spec.kind = p.kind;
            return spec;
        }
    }
    // A file in the plugin folder: relative, inside it, a picture Win顺 can
    // show (no SVG: the installed Qt has no SVG support; Manifest says so).
    if (!insideFolder(text))
        return spec;
    const QString lower = text.toLower();
    if (!lower.endsWith(u".png"_s) && !lower.endsWith(u".ico"_s) && !lower.endsWith(u".jpg"_s) && !lower.endsWith(u".jpeg"_s))
        return spec;
    spec.kind = IconSpec::Kind::PluginFile;
    spec.value = text;
    return spec;
}

namespace {

Open parseOpen(const QJsonValue& value)
{
    Open open;
    if (!value.isObject())
        return open;
    const QJsonObject o = value.toObject();
    if (o.value(u"url"_s).isString() && !o.value(u"url"_s).toString().isEmpty()) {
        open.kind = Open::Kind::Url;
        open.target = o.value(u"url"_s).toString();
    } else if (o.value(u"path"_s).isString() && !o.value(u"path"_s).toString().isEmpty()) {
        open.kind = Open::Kind::Path;
        open.target = o.value(u"path"_s).toString();
        open.args = o.value(u"args"_s).toString();
        open.cwd = o.value(u"cwd"_s).toString();
    }
    return open;
}

QJsonValue openToJson(const Open& open)
{
    switch (open.kind) {
    case Open::Kind::None: return {};
    case Open::Kind::Url: return QJsonObject {{u"url"_s, open.target}};
    case Open::Kind::Path: {
        QJsonObject o {{u"path"_s, open.target}};
        if (!open.args.isEmpty())
            o.insert(u"args"_s, open.args);
        if (!open.cwd.isEmpty())
            o.insert(u"cwd"_s, open.cwd);
        return o;
    }
    }
    return {};
}

QList<Action> parseActions(const QJsonValue& value)
{
    QList<Action> actions;
    for (const QJsonValue& a : value.toArray()) {
        if (!a.isObject())
            continue;
        const QJsonObject o = a.toObject();
        Action action;
        action.id = o.value(u"id"_s).toString();
        if (action.id.isEmpty())
            continue;
        const QJsonValue title = o.value(u"title"_s);
        if (title.isString())
            action.title = {title.toString(), title.toString()};
        else if (title.isObject())
            action.title = {title.toObject().value(u"zh"_s).toString(), title.toObject().value(u"en"_s).toString()};
        action.key = o.value(u"key"_s).toString();
        actions.push_back(action);
    }
    return actions;
}

} // namespace

Item parseItem(const QJsonObject& o)
{
    Item item;
    item.id = o.value(u"id"_s).toString();
    item.title = o.value(u"title"_s).toString();
    item.subtitle = o.value(u"subtitle"_s).toString();
    item.subtitleAway = o.value(u"subtitleAway"_s).toString();
    const auto time = [](const QJsonValue& value) -> std::uint32_t {
        const double seconds = value.isString()
            ? static_cast<double>(QDateTime::fromString(value.toString(), Qt::ISODateWithMs).toSecsSinceEpoch())
            : value.isDouble() ? value.toDouble() : 0;
        return std::isfinite(seconds) && seconds > 0 && seconds <= 0xFFFF'FFFF && std::floor(seconds) == seconds
            ? static_cast<std::uint32_t>(seconds) : 0;
    };
    item.modified = time(o.value(u"modified"_s));
    item.created = time(o.value(u"created"_s));
    const QJsonValue size = o.value(u"size"_s);
    if (size.isDouble()) {
        const double bytes = size.toDouble();
        if (std::isfinite(bytes) && bytes >= 0 && bytes <= 9'007'199'254'740'991.0 && std::floor(bytes) == bytes)
            item.size = static_cast<qint64>(bytes);
    }
    item.icon = parseIcon(o.value(u"icon"_s).toString());
    if (o.value(u"score"_s).isDouble()) {
        item.hasScore = true;
        item.score = std::clamp(static_cast<int>(o.value(u"score"_s).toDouble()), 0, 100);
    }
    item.match = o.value(u"match"_s).toString();
    item.autocomplete = o.value(u"autocomplete"_s).toString();
    item.open = parseOpen(o.value(u"open"_s));
    item.copy = o.value(u"copy"_s).toString();
    const QJsonValue preview = o.value(u"preview"_s);
    if (preview.isObject())
        item.previewText = preview.toObject().value(u"text"_s).toString();
    else if (preview.isString())
        item.previewText = preview.toString();
    item.actions = parseActions(o.value(u"actions"_s));
    item.valid = o.value(u"valid"_s).toBool(true);
    item.data = o.value(u"data"_s);
    const QJsonValue search = o.value(u"search"_s);
    if (search.isString())
        item.searchQuery = search.toString();
    else if (search.isObject()) {
        item.searchQuery = search.toObject().value(u"query"_s).toString();
        item.searchScope = search.toObject().value(u"scope"_s).toString();
    }
    item.setQuery = o.value(u"setQuery"_s).toString();
    return item;
}

QueryResult parseResult(const QJsonValue& value)
{
    QueryResult result;
    QJsonArray items;
    if (value.isArray())
        items = value.toArray();
    else if (value.isObject()) {
        const QJsonObject o = value.toObject();
        items = o.value(u"items"_s).toArray();
        result.rerun = std::max(0.0, o.value(u"rerun"_s).toDouble());
        result.cacheSeconds = std::max(0, static_cast<int>(o.value(u"cache"_s).toDouble()));
    } else
        return result;
    for (const QJsonValue& v : items) {
        if (!v.isObject())
            continue;
        if (result.items.size() >= kMaxItems) {
            result.truncated = true;
            break;
        }
        Item item = parseItem(v.toObject());
        if (item.title.isEmpty())
            continue;
        result.items.push_back(std::move(item));
    }
    return result;
}

QueryResult parseOutput(const QByteArray& text)
{
    const QByteArray trimmed = text.trimmed();
    if (trimmed.startsWith('{') || trimmed.startsWith('[')) {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(trimmed, &error);
        if (error.error == QJsonParseError::NoError)
            return parseResult(document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object()));
    }
    // Plain text: a row per line, Enter copies it.
    QueryResult result;
    for (QByteArray line : text.split('\n')) {
        if (line.endsWith('\r'))
            line.chop(1);
        const QString s = QString::fromUtf8(line).trimmed();
        if (s.isEmpty())
            continue;
        if (result.items.size() >= kMaxItems) {
            result.truncated = true;
            break;
        }
        Item item;
        item.title = s;
        item.copy = s;
        result.items.push_back(std::move(item));
    }
    return result;
}

Effects parseEffects(const QJsonValue& value)
{
    Effects effects;
    QJsonArray list;
    if (value.isArray())
        list = value.toArray();
    else if (value.isObject())
        list = value.toObject().value(u"effects"_s).toArray();
    for (const QJsonValue& v : list) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        const QString type = o.value(u"type"_s).toString();
        Effect effect;
        if (type == u"hide")
            effect.kind = Effect::Kind::Hide;
        else if (type == u"keep")
            effect.kind = Effect::Kind::Keep;
        else if (type == u"setQuery") {
            effect.kind = Effect::Kind::SetQuery;
            effect.text = o.value(u"text"_s).toString();
        } else if (type == u"notify") {
            effect.kind = Effect::Kind::Notify;
            effect.text = o.value(u"text"_s).toString();
        } else if (type == u"copy") {
            effect.kind = Effect::Kind::Copy;
            effect.text = o.value(u"text"_s).toString();
        } else if (type == u"open") {
            effect.kind = Effect::Kind::Open;
            effect.open = parseOpen(o);
            if (effect.open.kind == Open::Kind::None)
                continue;
        } else if (type == u"replace") {
            effect.kind = Effect::Kind::Replace;
            effect.items = parseResult(o.value(u"items"_s)).items;
        } else
            continue;
        effects.push_back(std::move(effect));
    }
    if (effects.isEmpty())
        effects.push_back(Effect {});
    return effects;
}

QJsonObject itemToJson(const Item& item)
{
    QJsonObject o;
    if (!item.id.isEmpty())
        o.insert(u"id"_s, item.id);
    o.insert(u"title"_s, item.title);
    if (!item.subtitle.isEmpty())
        o.insert(u"subtitle"_s, item.subtitle);
    if (!item.subtitleAway.isEmpty())
        o.insert(u"subtitleAway"_s, item.subtitleAway);
    if (item.modified != 0)
        o.insert(u"modified"_s, static_cast<qint64>(item.modified));
    if (item.created != 0)
        o.insert(u"created"_s, static_cast<qint64>(item.created));
    if (item.size >= 0)
        o.insert(u"size"_s, item.size);
    if (item.hasScore)
        o.insert(u"score"_s, item.score);
    if (!item.match.isEmpty())
        o.insert(u"match"_s, item.match);
    if (!item.autocomplete.isEmpty())
        o.insert(u"autocomplete"_s, item.autocomplete);
    if (item.open.kind != Open::Kind::None)
        o.insert(u"open"_s, openToJson(item.open));
    if (!item.copy.isEmpty())
        o.insert(u"copy"_s, item.copy);
    if (!item.valid)
        o.insert(u"valid"_s, false);
    if (!item.data.isUndefined() && !item.data.isNull())
        o.insert(u"data"_s, item.data);
    return o;
}

} // namespace ws::plugin
