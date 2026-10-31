#include "MovedPlugins.h"

#include "Catalog.h"
#include "Gallery.h"
#include "Install.h"
#include "platform/Http.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QThreadPool>

#include <windows.h>

#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;

namespace ws::moved {

namespace gallery = plugin::gallery;

namespace {

// Came with Win顺, in the library since (GitHub #138): 局域网传输 and 按意思找
// with 0.16.0, the screenshot as part of WinShun.exe, 翻译 and 浏览器书签 (in
// Python then, C++ now) since 0.7.0.
constexpr std::array<QStringView, 5> kMoved {u"winshun.localsend", u"winshun.semantic", kCapture, u"winshun.translate", u"winshun.bookmarks"};

constexpr int kListTimeoutMs = 15000; // as the 插件库 tab's
constexpr int kPackageTimeoutMs = 30000;

gallery::Fetcher webFetcher(int timeoutMs)
{
    return [timeoutMs](const QString& url, qint64 maxBytes, const gallery::Progress& progress) {
        http::Response r = http::get(url, {}, timeoutMs, maxBytes, http::Session::Own, progress);
        return gallery::Fetched {r.status, std::move(r.body), r.error};
    };
}

// No answer at all (not a file that is not there): the network, most likely.
bool offline(const gallery::Fetched& f)
{
    return f.status == 0 && f.error != ERROR_FILE_NOT_FOUND && f.error != ERROR_ACCESS_DENIED && f.error != ERROR_READ_FAULT;
}

Outcome installOne(const gallery::Index& index, const QString& id, const QString& userDir, const QString& locale)
{
    Outcome out;
    out.id = id;
    out.name = name(id);
    const gallery::Item* item = index.find(id);
    if (!item) {
        out.error = QCoreApplication::translate("MovedPlugins", "插件库里没有找到它");
        return out;
    }
    out.name = item->name.pick(locale);
    if (gallery::stateOf(*item, {}, QStringLiteral(WINSHUN_VERSION)) == gallery::State::NeedsNewerApp) {
        out.error = QCoreApplication::translate("MovedPlugins", "插件库里的版本要更新的 Win顺");
        return out;
    }
    if (item->size > gallery::kMaxPackageBytes) {
        out.error = QCoreApplication::translate("MovedPlugins", "插件包太大");
        return out;
    }
    // One byte more than the list says: a bigger package shows as one.
    const gallery::Fetched fetched = gallery::fetch(item->url, item->size + 1, webFetcher(kPackageTimeoutMs), {},
        [&](const QByteArray& bytes) { return gallery::check(*item, bytes) == gallery::Problem::None; });
    if (fetched.status != 200) {
        out.offline = offline(fetched);
        out.error = QCoreApplication::translate("MovedPlugins", "下载不下来（%1）").arg(fetched.status ? fetched.status : static_cast<int>(fetched.error));
        return out;
    }
    if (gallery::check(*item, fetched.body) != gallery::Problem::None) {
        out.error = QCoreApplication::translate("MovedPlugins", "下载的文件校验不对");
        return out;
    }
    QString error;
    const QString path = gallery::saveTemporary(*item, fetched.body, &error);
    if (path.isEmpty()) {
        out.error = error;
        return out;
    }
    std::optional<plugin::install::Candidate> candidate = plugin::install::inspect(path);
    QFile::remove(path); // read by now: an archive is unpacked into a folder of its own
    if (!candidate || !candidate->manifest) {
        out.error = candidate ? candidate->error : QCoreApplication::translate("MovedPlugins", "读不出下载的插件");
        if (candidate)
            plugin::install::discard(*candidate);
        return out;
    }
    if (candidate->manifest->id != id) {
        plugin::install::discard(*candidate);
        out.error = QCoreApplication::translate("MovedPlugins", "下载来的是另一个插件（%1）").arg(candidate->manifest->id);
        return out;
    }
    if (plugin::install::place(*candidate, userDir, &error).isEmpty())
        out.error = error;
    plugin::install::discard(*candidate);
    return out;
}

} // namespace

QStringList missing(const plugin::Catalog& catalog, const QList<PluginConfig>& plugins)
{
    QStringList ids;
    for (const QStringView id : kMoved) {
        const auto config = std::ranges::find_if(plugins, [&](const PluginConfig& c) { return c.id == id; });
        if (config != plugins.end() && config->enabled && !catalog.find(id))
            ids.push_back(id.toString());
    }
    return ids;
}

QString name(QStringView id)
{
    if (id == kCapture)
        return QCoreApplication::translate("MovedPlugins", "截图与录屏");
    if (id == u"winshun.localsend")
        return QCoreApplication::translate("MovedPlugins", "局域网传输");
    if (id == u"winshun.semantic")
        return QCoreApplication::translate("MovedPlugins", "按意思找");
    if (id == u"winshun.translate")
        return QCoreApplication::translate("MovedPlugins", "翻译");
    if (id == u"winshun.bookmarks")
        return QCoreApplication::translate("MovedPlugins", "浏览器书签");
    return id.toString();
}

QStringList capturePermissions()
{
    return {u"ui"_s, u"clipboard:read"_s, u"clipboard:write"_s, u"files:write"_s, u"notifications"_s, u"winshun:open"_s};
}

bool moveCaptureSettings(const QString& settingsFile, const QString& dataRoot)
{
    QSettings from(settingsFile, QSettings::IniFormat);
    from.beginGroup(u"Capture"_s);
    if (!from.value(u"Enabled"_s).toBool())
        return false;
    const QString folder = QDir(dataRoot).filePath(kCapture.toString() + u".data"_s);
    const QString file = QDir(folder).filePath(u"capture.ini"_s);
    const QStringList keys = from.childKeys();
    if (!QFileInfo::exists(file)) {
        if (!QDir().mkpath(folder))
            return false;
        QSettings to(file, QSettings::IniFormat);
        for (const QString& key : keys) {
            if (key != u"Enabled" && key != u"Keyword") // the switch is the plugin's now; the keyword stays Win顺's
                to.setValue(u"Capture/"_s + key, from.value(key));
        }
        to.sync();
        if (to.status() != QSettings::NoError)
            return false;
    }
    for (const QString& key : keys) {
        if (key != u"Keyword" && key != u"PrintScreenBefore")
            from.remove(key);
    }
    from.endGroup();
    // Publish the plugin's enabled state in the same atomic INI update as
    // removing the old switch. An interrupted start can then retry recovery.
    from.setValue(u"Plugin.winshun.capture/Enabled"_s, true);
    from.setValue(u"Plugin.winshun.capture/Confirmed"_s, capturePermissions());
    from.sync();
    return from.status() == QSettings::NoError;
}

void install(const QStringList& ids, const QString& userDir, const QString& locale, std::function<void(const QList<Outcome>&)> done)
{
    const QString given = qEnvironmentVariable("WINSHUN_PLUGIN_INDEX"); // a list made for a test, as the 插件库 tab takes
    const QString source = given.isEmpty() ? QString(gallery::kIndexUrl) : given;
    QThreadPool::globalInstance()->start([ids, userDir, locale, source, done = std::move(done)] {
        QList<Outcome> outcomes;
        const gallery::Fetched fetched = gallery::fetch(source, gallery::kMaxIndexBytes + 1, webFetcher(kListTimeoutMs), {},
            [&](const QByteArray& bytes) { return bytes.size() <= gallery::kMaxIndexBytes && gallery::parse(bytes, source, nullptr).has_value(); });
        QString error;
        std::optional<gallery::Index> index;
        if (fetched.status == 200 && fetched.body.size() <= gallery::kMaxIndexBytes)
            index = gallery::parse(fetched.body, source, &error);
        for (const QString& id : ids) {
            if (index) {
                outcomes.push_back(installOne(*index, id, userDir, locale));
                continue;
            }
            Outcome out;
            out.id = id;
            out.name = name(id);
            out.offline = offline(fetched);
            out.error = QCoreApplication::translate("MovedPlugins", "拿不到插件库的列表（%1）")
                            .arg(fetched.status ? QString::number(fetched.status) : error.isEmpty() ? QString::number(fetched.error) : error);
            outcomes.push_back(std::move(out));
        }
        QMetaObject::invokeMethod(qApp, [done, outcomes] { done(outcomes); }, Qt::QueuedConnection);
    });
}

} // namespace ws::moved
