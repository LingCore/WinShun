#include "DisplaySetup.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QString>
#include <QTimeZone>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

#include <windows.h>
#include <setupapi.h>
#include <winevt.h>

using namespace Qt::StringLiterals;

namespace ws::diagnostics {

namespace {

constexpr int kMostDriverValues = 120; // of one key's sweep
constexpr int kMostDriverKeys = 4000; // looked into, of one key's sweep
constexpr int kDriverDepth = 5; // AMD's FreeSync: DAL2_DATA__2_0\DisplayPath_n\EDID_…\Option\…
constexpr qsizetype kMostText = 120;
// GUID_DEVCLASS_DISPLAY (devguid.h), here to need no initguid.h.
constexpr GUID kDisplayClass {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

QString connectionName(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology)
{
    switch (technology) {
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15: return u"VGA"_s;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI: return u"DVI"_s;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI: return u"HDMI"_s;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS: return u"LVDS"_s;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL: return u"DisplayPort"_s;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED: return u"DisplayPort (embedded)"_s;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EXTERNAL: return u"UDI"_s;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED: return u"UDI (embedded)"_s;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST: return u"Miracast"_s;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED: return u"indirect (wired)"_s;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL: return u"indirect (virtual)"_s;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL: return u"internal"_s;
    default: return QString::number(static_cast<qint64>(technology));
    }
}

// "GSM 5B09": the maker's three letters (big-endian in the EDID) and the product code.
QString edidName(UINT16 manufacturer, UINT16 product)
{
    const auto id = static_cast<UINT16>((manufacturer >> 8) | (manufacturer << 8));
    const auto letter = [id](int shift) { return QChar(static_cast<char16_t>(u'@' + ((id >> shift) & 31))); };
    const QChar maker[] {letter(10), letter(5), letter(0)};
    return QString(maker, 3) + u' ' + u"%1"_s.arg(product, 4, 16, QChar(u'0')).toUpper();
}

template <typename Info>
bool ask(Info& info, DISPLAYCONFIG_DEVICE_INFO_TYPE type, LUID adapter, UINT32 id)
{
    info.header.type = type;
    info.header.size = sizeof info;
    info.header.adapterId = adapter;
    info.header.id = id;
    return ::DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS;
}

// A registry value as text: a number, a string, or bytes (UTF-16 text when
// they are some, as AMD keeps its switches; otherwise hex, the first 32).
QString valueText(DWORD type, const BYTE* data, DWORD size)
{
    switch (type) {
    case REG_DWORD:
        if (size >= sizeof(DWORD)) {
            DWORD value = 0;
            std::memcpy(&value, data, sizeof value);
            return QString::number(value);
        }
        break;
    case REG_QWORD:
        if (size >= sizeof(quint64)) {
            quint64 value = 0;
            std::memcpy(&value, data, sizeof value);
            return QString::number(value);
        }
        break;
    case REG_SZ:
    case REG_EXPAND_SZ:
    case REG_MULTI_SZ: {
        QString text = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(data), static_cast<qsizetype>(size / 2));
        text.replace(QChar(0), u';');
        while (text.endsWith(u';'))
            text.chop(1);
        return text.left(kMostText);
    }
    case REG_BINARY: {
        if (size % 2 == 0 && size > 0 && size <= 256) {
            QString text = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(data), static_cast<qsizetype>(size / 2));
            while (text.endsWith(QChar(0)))
                text.chop(1);
            if (!text.isEmpty() && std::ranges::all_of(text, [](QChar c) { return c.unicode() >= 0x20 && c.unicode() < 0x7f; }))
                return text.left(kMostText);
        }
        return QString::fromLatin1(
            QByteArray(reinterpret_cast<const char*>(data), static_cast<qsizetype>(std::min<DWORD>(size, 32))).toHex(' '));
    }
    default:
        break;
    }
    return u"type %1, %2 bytes"_s.arg(type).arg(size);
}

std::optional<QString> registryText(HKEY root, const wchar_t* key, const wchar_t* name)
{
    DWORD type = 0;
    DWORD size = 0;
    if (::RegGetValueW(root, key, name, RRF_RT_ANY | RRF_NOEXPAND, &type, nullptr, &size) != ERROR_SUCCESS)
        return std::nullopt;
    std::vector<BYTE> data(size + sizeof(wchar_t));
    if (::RegGetValueW(root, key, name, RRF_RT_ANY | RRF_NOEXPAND, &type, data.data(), &size) != ERROR_SUCCESS)
        return std::nullopt;
    return valueText(type, data.data(), size);
}

} // namespace

bool isPacingSwitch(QStringView name)
{
    // Case-sensitive where a short name would be found inside others' words
    // ("RmPowerFeature", "DListDriverName", "AsyncCompute").
    static const QRegularExpression pacing(
        u"(?i:(?<!a)sync|vrr|chill|anti_?lag|boost|hypr|afmf|fluid|frtc|flip_?q|low_?latency|overlay|framepac"
        u"|ulps|deep_?sleep|stutter)|MPO|Mpo(?![a-z])|TDR|Tdr|PSR(?![a-z])"_s);
    return pacing.matchView(name).hasMatch();
}

namespace {

void sweep(HKEY key, const QString& path, int depth, QJsonObject& found, int& keysLeft)
{
    std::array<wchar_t, 256> name {};
    std::array<BYTE, 512> data {};
    for (DWORD i = 0;; ++i) {
        DWORD nameLength = static_cast<DWORD>(name.size());
        DWORD type = 0;
        DWORD size = static_cast<DWORD>(data.size());
        const LSTATUS status = ::RegEnumValueW(key, i, name.data(), &nameLength, nullptr, &type, data.data(), &size);
        if (status == ERROR_MORE_DATA) // a big value is no switch
            continue;
        if (status != ERROR_SUCCESS)
            break;
        const QString valueName = QString::fromWCharArray(name.data(), static_cast<qsizetype>(nameLength));
        if (!isPacingSwitch(valueName))
            continue;
        if (found.size() >= kMostDriverValues)
            return;
        found[path.isEmpty() ? valueName : path + u'\\' + valueName] = valueText(type, data.data(), size);
    }
    if (depth == 0)
        return;
    for (DWORD i = 0; keysLeft > 0; ++i) {
        DWORD nameLength = static_cast<DWORD>(name.size());
        if (::RegEnumKeyExW(key, i, name.data(), &nameLength, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        --keysLeft;
        HKEY child = nullptr;
        if (::RegOpenKeyExW(key, name.data(), 0, KEY_READ, &child) != ERROR_SUCCESS)
            continue;
        const QString childName = QString::fromWCharArray(name.data(), static_cast<qsizetype>(nameLength));
        sweep(child, path.isEmpty() ? childName : path + u'\\' + childName, depth - 1, found, keysLeft);
        ::RegCloseKey(child);
    }
}

QJsonObject swept(HKEY key, int depth)
{
    QJsonObject found;
    int keysLeft = kMostDriverKeys;
    sweep(key, {}, depth, found, keysLeft);
    return found;
}

QString isoUtc(ULONGLONG fileTime)
{
    constexpr ULONGLONG kUnixEpoch = 116'444'736'000'000'000ull; // 1970 in 100 ns since 1601
    if (fileTime < kUnixEpoch)
        return {};
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>((fileTime - kUnixEpoch) / 10'000), QTimeZone::UTC)
        .toString(Qt::ISODateWithMs);
}

} // namespace

QJsonArray monitors()
{
    QJsonArray out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG status = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 3 && status == ERROR_INSUFFICIENT_BUFFER; ++attempt) { // changed in between: again
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
            return out;
        paths.resize(pathCount);
        modes.resize(modeCount);
        status = ::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        paths.resize(pathCount);
        modes.resize(modeCount);
    }
    if (status != ERROR_SUCCESS)
        return out;
    MONITORINFOEXW main {};
    main.cbSize = sizeof main;
    ::GetMonitorInfoW(::MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &main);
    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        QJsonObject monitor;
        if (DISPLAYCONFIG_TARGET_DEVICE_NAME target {};
            ask(target, DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, path.targetInfo.adapterId, path.targetInfo.id)) {
            if (const QString name = QString::fromWCharArray(target.monitorFriendlyDeviceName); !name.isEmpty())
                monitor[u"name"_s] = name; // none for most laptops' own screens
            if (target.flags.edidIdsValid)
                monitor[u"edid"_s] = edidName(target.edidManufactureId, target.edidProductCodeId);
        }
        monitor[u"connection"_s] = connectionName(path.targetInfo.outputTechnology);
        if (const DISPLAYCONFIG_RATIONAL rate = path.targetInfo.refreshRate; rate.Denominator)
            monitor[u"hz"_s] = std::round(static_cast<double>(rate.Numerator) / rate.Denominator * 100.0) / 100.0;
        if (const UINT32 at = path.sourceInfo.modeInfoIdx; at < modes.size() && modes[at].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
            monitor[u"size"_s] = u"%1x%2"_s.arg(modes[at].sourceMode.width).arg(modes[at].sourceMode.height);
        if (DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
            ask(source, DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, path.sourceInfo.adapterId, path.sourceInfo.id))
            monitor[u"primary"_s] = std::wcscmp(source.viewGdiDeviceName, main.szDevice) == 0;
        if (DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color {};
            ask(color, DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, path.targetInfo.adapterId, path.targetInfo.id))
            monitor[u"hdr"_s] = static_cast<bool>(color.advancedColorEnabled);
        out.append(monitor);
    }
    return out;
}

QJsonObject graphicsSettings()
{
    struct Setting {
        QString name;
        HKEY root;
        const wchar_t* key;
        const wchar_t* value;
    };
    constexpr wchar_t kDrivers[] = L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers";
    const Setting settings[] {
        {u"hags"_s, HKEY_LOCAL_MACHINE, kDrivers, L"HwSchMode"}, // 2: on, 1: off
        {u"tdr_delay"_s, HKEY_LOCAL_MACHINE, kDrivers, L"TdrDelay"}, // seconds; absent: 2
        {u"tdr_level"_s, HKEY_LOCAL_MACHINE, kDrivers, L"TdrLevel"}, // 0: off; absent: 3, recover
        {u"mpo_overlay_test_mode"_s, HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\Dwm", L"OverlayTestMode"}, // 5: off
        {u"directx_user"_s, HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", L"DirectXUserGlobalSettings"},
        {u"game_mode"_s, HKEY_CURRENT_USER, L"Software\\Microsoft\\GameBar", L"AutoGameModeEnabled"},
        {u"game_dvr"_s, HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_Enabled"},
        {u"app_capture"_s, HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR", L"AppCaptureEnabled"},
    };
    QJsonObject out;
    for (const Setting& setting : settings) {
        if (const std::optional<QString> text = registryText(setting.root, setting.key, setting.value))
            out[setting.name] = *text;
    }
    return out;
}

QJsonObject driverSettings()
{
    QJsonObject out;
    const HDEVINFO devices = ::SetupDiGetClassDevsW(&kDisplayClass, nullptr, nullptr, DIGCF_PRESENT);
    if (devices != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA device {};
        device.cbSize = sizeof device;
        for (DWORD i = 0; ::SetupDiEnumDeviceInfo(devices, i, &device); ++i) {
            std::array<wchar_t, 256> description {};
            ::SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_DEVICEDESC, nullptr,
                reinterpret_cast<PBYTE>(description.data()), static_cast<DWORD>(description.size() * sizeof(wchar_t)), nullptr);
            const HKEY key = ::SetupDiOpenDevRegKey(devices, &device, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
            if (key == INVALID_HANDLE_VALUE)
                continue;
            QString name = QString::fromWCharArray(description.data());
            if (name.isEmpty() || out.contains(name))
                name += u" #%1"_s.arg(i);
            out[name] = swept(key, kDriverDepth);
            ::RegCloseKey(key);
        }
        ::SetupDiDestroyDeviceInfoList(devices);
    }
    HKEY amd = nullptr; // Adrenalin's own, per user
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AMD\\CN", 0, KEY_READ, &amd) == ERROR_SUCCESS) {
        if (QJsonObject found = swept(amd, 3); !found.isEmpty())
            out[u"AMD Software (user)"_s] = found;
        ::RegCloseKey(amd);
    }
    return out;
}

QJsonArray displayEvents(int days, int most)
{
    QJsonArray out;
    const QString query = u"*[System[Provider[@Name='Display' or @Name='Microsoft-Windows-DxgKrnl' or @Name='amdkmdag' "
                          u"or @Name='amdwddmg' or @Name='nvlddmkm' or @Name='igfx' or @Name='igfxn' or @Name='igfxnd'] "
                          u"and (Level=1 or Level=2 or Level=3) and TimeCreated[timediff(@SystemTime) <= %1]]]"_s.arg(static_cast<qint64>(days) * 86'400'000);
    const EVT_HANDLE results = ::EvtQuery(nullptr, L"System", reinterpret_cast<LPCWSTR>(query.utf16()),
        EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!results)
        return out;
    const EVT_HANDLE context = ::EvtCreateRenderContext(0, nullptr, EvtRenderContextSystem);
    std::array<EVT_HANDLE, 16> events {};
    std::vector<BYTE> buffer(8192);
    while (context && out.size() < most) {
        DWORD returned = 0;
        if (!::EvtNext(results, static_cast<DWORD>(events.size()), events.data(), 2000, 0, &returned))
            break;
        for (DWORD i = 0; i < returned; ++i) {
            DWORD used = 0;
            DWORD count = 0;
            BOOL rendered = ::EvtRender(context, events[i], EvtRenderEventValues, static_cast<DWORD>(buffer.size()),
                buffer.data(), &used, &count);
            if (!rendered && ::GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                buffer.resize(used);
                rendered = ::EvtRender(context, events[i], EvtRenderEventValues, static_cast<DWORD>(buffer.size()),
                    buffer.data(), &used, &count);
            }
            if (rendered && count > EvtSystemTimeCreated && out.size() < most) {
                const auto* values = reinterpret_cast<const EVT_VARIANT*>(buffer.data());
                const EVT_VARIANT& provider = values[EvtSystemProviderName];
                out.append(QJsonObject {
                    {u"time"_s, values[EvtSystemTimeCreated].Type == EvtVarTypeFileTime ? isoUtc(values[EvtSystemTimeCreated].FileTimeVal) : QString()},
                    {u"provider"_s, provider.Type == EvtVarTypeString ? QString::fromWCharArray(provider.StringVal) : QString()},
                    {u"id"_s, values[EvtSystemEventID].Type == EvtVarTypeUInt16 ? values[EvtSystemEventID].UInt16Val : -1},
                    {u"level"_s, values[EvtSystemLevel].Type == EvtVarTypeByte ? values[EvtSystemLevel].ByteVal : -1}});
            }
            ::EvtClose(events[i]);
        }
    }
    if (context)
        ::EvtClose(context);
    ::EvtClose(results);
    return out;
}

} // namespace ws::diagnostics
