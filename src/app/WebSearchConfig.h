#pragma once

#include "Settings.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

namespace ws::webplugin {

inline constexpr QStringView kId = u"winshun.web";

inline const QString* value(const PluginConfig& config, QStringView key)
{
    for (const auto& setting : config.settings) {
        if (setting.first == key)
            return &setting.second;
    }
    return nullptr;
}

inline void setValue(PluginConfig& config, const QString& key, const QString& text)
{
    for (auto& setting : config.settings) {
        if (setting.first == key) {
            setting.second = text;
            return;
        }
    }
    config.settings.append({key, text});
}

// Local configuration keeps icon references; portable JSON exports embed the
// pictures instead. An explicitly empty array stays empty across upgrades.
inline WebShortcuts shortcuts(const PluginConfig* config)
{
    const QString* text = config ? value(*config, u"shortcuts") : nullptr;
    if (!text)
        return defaultWebShortcuts();
    const auto document = QJsonDocument::fromJson(text->toUtf8());
    WebShortcuts result;
    for (const auto& item : document.array()) {
        const auto object = item.toObject();
        WebShortcut shortcut;
        shortcut.keyword = object.value(QStringLiteral("keyword")).toString().trimmed();
        shortcut.name = object.value(QStringLiteral("name")).toString().trimmed();
        shortcut.url = normalizeWebUrl(object.value(QStringLiteral("url")).toString());
        shortcut.home = normalizeWebUrl(object.value(QStringLiteral("home")).toString());
        const QString icon = object.value(QStringLiteral("icon")).toString();
        if (isValidCustomIconName(icon))
            shortcut.icon = icon;
        shortcut.suggest = normalizeSuggestUrl(object.value(QStringLiteral("suggest")).toString());
        if (!isValidWebKeyword(shortcut.keyword) || shortcut.url.isEmpty())
            continue;
        if (shortcut.name.isEmpty())
            shortcut.name = defaultWebShortcutName(shortcut);
        result.append(std::move(shortcut));
    }
    return result;
}

inline bool suggestions(const PluginConfig* config)
{
    const auto* text = config ? value(*config, u"suggestions") : nullptr;
    return !text || *text != u"false";
}

// "fallback": with nothing found on this PC, a row searching the first site
// for what was typed (GitHub #320). On unless switched off.
inline bool fallback(const PluginConfig* config)
{
    const auto* text = config ? value(*config, u"fallback") : nullptr;
    return !text || *text != u"false";
}

inline void store(PluginConfig& config, const WebShortcuts& shortcuts, bool suggestions, bool fallback)
{
    QJsonArray entries;
    for (const auto& shortcut : shortcuts) {
        entries.append(QJsonObject {{QStringLiteral("keyword"), shortcut.keyword}, {QStringLiteral("name"), shortcut.name},
            {QStringLiteral("url"), shortcut.url}, {QStringLiteral("home"), shortcut.home},
            {QStringLiteral("icon"), shortcut.icon}, {QStringLiteral("suggest"), shortcut.suggest}});
    }
    setValue(config, QStringLiteral("shortcuts"), QString::fromUtf8(QJsonDocument(entries).toJson(QJsonDocument::Compact)));
    setValue(config, QStringLiteral("suggestions"), suggestions ? QStringLiteral("true") : QStringLiteral("false"));
    setValue(config, QStringLiteral("fallback"), fallback ? QStringLiteral("true") : QStringLiteral("false"));
}

// The v1 zero-code sample's commands, including renamed/removed keywords.
// Built-in shortcuts keep ownership when both lists claim the same word.
inline void mergeLegacyCommands(WebShortcuts& shortcuts, const PluginConfig& config, const QJsonArray& manifestCommands = {})
{
    if (!config.enabled)
        return;
    const WebShortcuts legacy {
        {QStringLiteral("bd"), QStringLiteral("百度"), QStringLiteral("https://www.baidu.com/s?wd=%s"), QStringLiteral("https://www.baidu.com/")},
        {QStringLiteral("gh"), QStringLiteral("GitHub"), QStringLiteral("https://github.com/search?q=%s"), QStringLiteral("https://github.com/")},
        {QStringLiteral("yt"), QStringLiteral("YouTube"), QStringLiteral("https://www.youtube.com/results?search_query=%s"), QStringLiteral("https://www.youtube.com/")},
        {QStringLiteral("wiki"), QStringLiteral("维基百科"), QStringLiteral("https://zh.wikipedia.org/w/index.php?search=%s"), QStringLiteral("https://zh.wikipedia.org/")},
        {QStringLiteral("bing"), QStringLiteral("必应"), QStringLiteral("https://www.bing.com/search?q=%s"), QStringLiteral("https://www.bing.com/")},
        {QStringLiteral("bili"), QStringLiteral("哔哩哔哩"), QStringLiteral("https://search.bilibili.com/all?keyword=%s"), QStringLiteral("https://www.bilibili.com/")}
    };
    QList<std::pair<QString, WebShortcut>> entries;
    if (manifestCommands.isEmpty()) {
        for (const auto& shortcut : legacy)
            entries.append({shortcut.keyword, shortcut});
    } else {
        for (const auto& entry : manifestCommands) {
            const auto command = entry.toObject();
            const auto run = command.value(QStringLiteral("run")).toObject();
            WebShortcut shortcut;
            shortcut.url = normalizeWebUrl(run.value(QStringLiteral("url")).toString());
            shortcut.home = normalizeWebUrl(run.value(QStringLiteral("home")).toString());
            if (shortcut.url.isEmpty())
                continue;
            const auto title = command.value(QStringLiteral("title"));
            shortcut.name = title.isObject() ? title.toObject().value(QStringLiteral("zh")).toString() : title.toString();
            if (shortcut.name.isEmpty() && title.isObject())
                shortcut.name = title.toObject().value(QStringLiteral("en")).toString();
            const auto keyword = command.value(QStringLiteral("keyword"));
            const auto keywords = keyword.isArray() ? keyword.toArray() : QJsonArray {keyword};
            for (const auto& key : keywords) {
                shortcut.keyword = key.toString().trimmed();
                entries.append({command.value(QStringLiteral("id")).toString(), shortcut});
            }
        }
    }
    for (auto [command, shortcut] : entries) {
        for (const auto& override : config.keywords) {
            if (override.first == command)
                shortcut.keyword = override.second;
        }
        if (!isValidWebKeyword(shortcut.keyword))
            continue;
        if (shortcut.name.isEmpty())
            shortcut.name = defaultWebShortcutName(shortcut);
        const bool taken = std::any_of(shortcuts.cbegin(), shortcuts.cend(), [&](const WebShortcut& own) {
            return own.keyword.compare(shortcut.keyword, Qt::CaseInsensitive) == 0;
        });
        if (!taken) {
            shortcut.suggest = knownSuggestUrl(shortcut.url);
            shortcuts.append(std::move(shortcut));
        }
    }
}

} // namespace ws::webplugin
