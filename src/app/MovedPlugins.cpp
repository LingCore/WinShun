#include "MovedPlugins.h"

#include "Catalog.h"
#include "Gallery.h"
#include "Install.h"
#include "platform/Http.h"

#include <QCoreApplication>
#include <QFile>
#include <QThreadPool>

#include <windows.h>

#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;

namespace ws::moved {

namespace gallery = plugin::gallery;

namespace {

// Came with 0.16.0, in the library since (GitHub #138).
constexpr std::array kMoved {u"winshun.localsend", u"winshun.semantic"};

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
    out.name = id;
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
    const gallery::Fetched fetched = gallery::fetch(item->url, item->size + 1, webFetcher(kPackageTimeoutMs));
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

void install(const QStringList& ids, const QString& userDir, const QString& locale, std::function<void(const QList<Outcome>&)> done)
{
    const QString given = qEnvironmentVariable("WINSHUN_PLUGIN_INDEX"); // a list made for a test, as the 插件库 tab takes
    const QString source = given.isEmpty() ? QString(gallery::kIndexUrl) : given;
    QThreadPool::globalInstance()->start([ids, userDir, locale, source, done = std::move(done)] {
        QList<Outcome> outcomes;
        const gallery::Fetched fetched = gallery::fetch(source, gallery::kMaxIndexBytes + 1, webFetcher(kListTimeoutMs));
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
            out.id = out.name = id;
            out.offline = offline(fetched);
            out.error = QCoreApplication::translate("MovedPlugins", "拿不到插件库的列表（%1）")
                            .arg(fetched.status ? QString::number(fetched.status) : error.isEmpty() ? QString::number(fetched.error) : error);
            outcomes.push_back(std::move(out));
        }
        QMetaObject::invokeMethod(qApp, [done, outcomes] { done(outcomes); }, Qt::QueuedConnection);
    });
}

} // namespace ws::moved
