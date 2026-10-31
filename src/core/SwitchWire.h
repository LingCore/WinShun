#pragma once

#include "SwitchIntent.h"
#include "SwitchState.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>
#include <QStringView>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace ws::switchwire {

inline constexpr QStringView kPlugin = u"winshun.switches";
inline constexpr std::array<QStringView, 7> kIds {
    u"theme", u"hidden-files", u"file-extensions", u"bluetooth", u"wlan", u"volume", u"keep-awake"};
inline constexpr std::array<QStringView, 9> kIntents {
    u"toggle", u"on", u"off", u"set-level", u"step-level", u"awake-for", u"awake-until", u"open-settings", u"level-only"};

inline bool known(QStringView id) { return std::ranges::find(kIds, id) != kIds.end(); }
inline QStringList ids()
{
    QStringList out;
    for (const QStringView id : kIds)
        out.append(id.toString());
    return out;
}

inline QJsonObject state(const SwitchState& value)
{
    QJsonObject out {{QStringLiteral("availability"), int(value.availability)}, {QStringLiteral("on"), int(value.on)},
        {QStringLiteral("level"), int(value.level)}, {QStringLiteral("note"), value.note}, {QStringLiteral("error"), value.error},
        {QStringLiteral("openInstead"), value.openInstead}, {QStringLiteral("mixedToggleOn"), value.mixedToggleOn},
        {QStringLiteral("carriesInternet"), value.carriesInternet}};
    if (value.until.isValid())
        out.insert(QStringLiteral("until"), static_cast<double>(value.until.toMSecsSinceEpoch()));
    return out;
}

inline std::optional<SwitchState> state(const QJsonValue& json)
{
    if (!json.isObject())
        return std::nullopt;
    const QJsonObject o = json.toObject();
    const int availability = o.value(QStringLiteral("availability")).toInt(-1);
    const int on = o.value(QStringLiteral("on")).toInt(-99);
    const int level = o.value(QStringLiteral("level")).toInt(-2);
    if (availability < 0 || availability > int(SwitchState::Availability::Unsupported) || on < -1 || on > 2 || level < -1 || level > 100)
        return std::nullopt;
    SwitchState out;
    out.availability = static_cast<SwitchState::Availability>(availability);
    out.on = static_cast<SwitchState::Value>(on);
    out.level = static_cast<qint8>(level);
    for (const auto& [key, target] : std::array<std::pair<QStringView, QString*>, 3> {{
             {u"note", &out.note}, {u"error", &out.error}, {u"openInstead", &out.openInstead}}}) {
        const QJsonValue text = o.value(key.toString());
        if (!text.isString() || text.toString().size() > 1024)
            return std::nullopt;
        *target = text.toString();
    }
    // A provider without processes permission must not turn fallback pages
    // into an indirect way of starting arbitrary programs.
    if (!out.openInstead.isEmpty() && !out.openInstead.startsWith(QStringLiteral("ms-settings:"), Qt::CaseInsensitive))
        return std::nullopt;
    if (!o.value(QStringLiteral("mixedToggleOn")).isBool() || !o.value(QStringLiteral("carriesInternet")).isBool())
        return std::nullopt;
    out.mixedToggleOn = o.value(QStringLiteral("mixedToggleOn")).toBool();
    out.carriesInternet = o.value(QStringLiteral("carriesInternet")).toBool();
    if (o.contains(QStringLiteral("until"))) {
        const QJsonValue until = o.value(QStringLiteral("until"));
        if (!until.isDouble() || until.toDouble() < 0 || until.toDouble() > 8e15)
            return std::nullopt;
        out.until = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(until.toDouble()));
        if (!out.until.isValid())
            return std::nullopt;
    }
    return out;
}

inline QJsonObject intent(const SwitchIntent& value)
{
    return {{QStringLiteral("kind"), kIntents.at(static_cast<std::size_t>(value.kind)).toString()},
        {QStringLiteral("value"), value.value}};
}

inline std::optional<SwitchIntent> intent(const QJsonValue& json)
{
    if (!json.isObject())
        return std::nullopt;
    const QJsonObject o = json.toObject();
    const QString kind = o.value(QStringLiteral("kind")).toString();
    const auto at = std::ranges::find(kIntents, QStringView(kind));
    if (at == kIntents.end())
        return std::nullopt;
    const int value = o.value(QStringLiteral("value")).toInt(-100001);
    if (value < -100000 || value > 100000)
        return std::nullopt;
    return SwitchIntent {static_cast<SwitchIntent::Kind>(at - kIntents.begin()), value};
}

inline QJsonObject write(const SwitchWrite& value)
{
    return {{QStringLiteral("ok"), value.ok}, {QStringLiteral("wrote"), value.wrote},
        {QStringLiteral("error"), value.error}, {QStringLiteral("after"), state(value.after)}};
}

inline std::optional<SwitchWrite> write(const QJsonValue& json)
{
    if (!json.isObject())
        return std::nullopt;
    const QJsonObject o = json.toObject();
    const auto after = state(o.value(QStringLiteral("after")));
    const QJsonValue error = o.value(QStringLiteral("error"));
    if (!after || !o.value(QStringLiteral("ok")).isBool() || !o.value(QStringLiteral("wrote")).isBool()
        || !error.isString() || error.toString().size() > 1024)
        return std::nullopt;
    return SwitchWrite {o.value(QStringLiteral("ok")).toBool(), o.value(QStringLiteral("wrote")).toBool(), error.toString(), *after};
}

} // namespace ws::switchwire
