#include "Secrets.h"

#include <windows.h>
#include <wincred.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace Qt::StringLiterals;

namespace ws::plugin::secrets {

namespace {

constexpr QStringView kPrefix = u"WinShun/plugin/";

} // namespace

QString target(const QString& pluginId, const QString& key)
{
    return kPrefix + pluginId + u'/' + key;
}

bool write(const QString& pluginId, const QString& key, const QString& value)
{
    if (value.isEmpty())
        return remove(pluginId, key);
    std::wstring name = target(pluginId, key).toStdWString();
    std::wstring user = L"WinShun";
    // UTF-16 as the blob, as Explorer keeps its passwords; 512 * 5 bytes is
    // the most a generic credential holds.
    std::wstring blob = value.toStdWString();
    if (blob.size() * sizeof(wchar_t) > CRED_MAX_CREDENTIAL_BLOB_SIZE)
        return false;
    CREDENTIALW credential {};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = name.data();
    credential.UserName = user.data();
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
    credential.CredentialBlobSize = static_cast<DWORD>(blob.size() * sizeof(wchar_t));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    const bool ok = ::CredWriteW(&credential, 0) != FALSE;
    ::SecureZeroMemory(blob.data(), blob.size() * sizeof(wchar_t));
    return ok;
}

std::optional<QString> read(const QString& pluginId, const QString& key)
{
    const std::wstring name = target(pluginId, key).toStdWString();
    PCREDENTIALW credential = nullptr;
    if (!::CredReadW(name.c_str(), CRED_TYPE_GENERIC, 0, &credential))
        return std::nullopt;
    QString value;
    if (credential->CredentialBlob && credential->CredentialBlobSize >= sizeof(wchar_t))
        value = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(credential->CredentialBlob),
            static_cast<qsizetype>(credential->CredentialBlobSize / sizeof(wchar_t)));
    ::SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
    ::CredFree(credential);
    return value;
}

bool remove(const QString& pluginId, const QString& key)
{
    const std::wstring name = target(pluginId, key).toStdWString();
    if (::CredDeleteW(name.c_str(), CRED_TYPE_GENERIC, 0))
        return true;
    return ::GetLastError() == ERROR_NOT_FOUND; // gone is what was asked for
}

QStringList keysOf(const QString& pluginId)
{
    QStringList keys;
    const std::wstring filter = (kPrefix + pluginId + u"/*"_s).toStdWString();
    DWORD count = 0;
    PCREDENTIALW* credentials = nullptr;
    if (!::CredEnumerateW(filter.c_str(), 0, &count, &credentials))
        return keys;
    const QString prefix = kPrefix + pluginId + u'/';
    for (DWORD i = 0; i < count; ++i) {
        if (credentials[i]->Type != CRED_TYPE_GENERIC || !credentials[i]->TargetName)
            continue;
        const QString name = QString::fromWCharArray(credentials[i]->TargetName);
        if (name.startsWith(prefix, Qt::CaseInsensitive))
            keys.push_back(name.mid(prefix.size()));
    }
    ::CredFree(credentials);
    return keys;
}

int removeAll(const QString& pluginId)
{
    int removed = 0;
    for (const QString& key : keysOf(pluginId))
        removed += remove(pluginId, key) ? 1 : 0;
    return removed;
}

namespace {

const Setting* declared(const Manifest& manifest, const QString& key)
{
    for (const Setting& s : manifest.settings) {
        if (s.key == key)
            return &s;
    }
    return nullptr;
}

} // namespace

bool store(const QString& pluginId, const Manifest& manifest, const QString& key, const QString& value,
    QList<std::pair<QString, QString>>& iniValues)
{
    const Setting* setting = declared(manifest, key);
    const auto drop = [&] {
        const qsizetype had = iniValues.size();
        iniValues.removeIf([&](const std::pair<QString, QString>& v) { return v.first == key; });
        return iniValues.size() != had;
    };
    if (setting && setting->type == u"password") {
        write(pluginId, key, value);
        return drop(); // never in the INI, not even from before the manifest called it one
    }
    QString text = value;
    if (setting && setting->type == u"number" && setting->bounded()) {
        // In its range, on its steps (GitHub #359), whoever set it; no
        // number at all: back to its default.
        bool ok = false;
        const double number = value.trimmed().toDouble(&ok);
        if (!ok || !std::isfinite(number))
            return drop();
        text = setting->numberText(number);
    }
    for (auto& [k, v] : iniValues) {
        if (k == key) {
            if (v == text)
                return false;
            v = text;
            return true;
        }
    }
    iniValues.append({key, text});
    return true;
}

QList<std::pair<QString, QString>> acceptedUpdate(const Manifest& manifest, const QJsonObject& values)
{
    QList<std::pair<QString, QString>> accepted;
    for (const Setting& setting : manifest.settings) {
        const QJsonValue value = values.value(setting.key);
        if (value.isUndefined())
            continue;
        if (setting.type == u"bool") {
            if (value.isBool())
                accepted.append({setting.key, value.toBool() ? u"true"_s : u"false"_s});
        } else if (setting.type == u"number") {
            if (value.isDouble() && std::isfinite(value.toDouble()))
                accepted.append({setting.key, QString::number(value.toDouble(), 'g', 17)});
        } else if (setting.type == u"choice") {
            // An option is its value, or an object with one.
            const bool offered = value.isString() && std::ranges::any_of(setting.options, [&](const QJsonValue& option) {
                return (option.isObject() ? option.toObject().value(u"value"_s) : option).toString() == value.toString();
            });
            if (offered)
                accepted.append({setting.key, value.toString()});
        } else if (setting.type == u"text") {
            if (value.isString() && value.toString().size() <= kSettingTextMost)
                accepted.append({setting.key, value.toString()});
        }
    }
    return accepted;
}

QJsonObject settingsOf(const QString& pluginId, const Manifest& manifest, const QList<std::pair<QString, QString>>& iniValues)
{
    QJsonObject settings;
    for (const auto& [key, value] : iniValues) {
        const Setting* s = declared(manifest, key);
        const QString type = s ? s->type : QString();
        if (type == u"password")
            continue; // the INI never holds one; the Credential Manager does
        if (type == u"bool")
            settings.insert(key, value.compare(u"true", Qt::CaseInsensitive) == 0 || value == u"1");
        else if (type == u"number" && s->bounded()) {
            // Kept before the manifest had its range, or typed into the file:
            // fitted all the same; no number: its default stands.
            bool ok = false;
            const double number = value.trimmed().toDouble(&ok);
            if (ok && std::isfinite(number))
                settings.insert(key, s->fit(number));
        } else if (type == u"number")
            settings.insert(key, value.toDouble());
        else
            settings.insert(key, value);
    }
    for (const Setting& s : manifest.settings) {
        if (s.type != u"password")
            continue;
        if (const std::optional<QString> secret = read(pluginId, s.key))
            settings.insert(s.key, *secret);
    }
    return settings;
}

} // namespace ws::plugin::secrets
