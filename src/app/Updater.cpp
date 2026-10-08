#include "Updater.h"

#include "Settings.h"
#include "platform/Http.h"
#include "platform/Shell.h"

#include <QCoreApplication>
#include <QPointer>
#include <QSettings>
#include <QThreadPool>
#include <QVariantMap>

#include <winhttp.h>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws {

namespace {

// Checked at every start, then every 12 hours (looked at hourly, so a
// sleeping PC catches up when it wakes).
constexpr auto kInterval = std::chrono::hours(12);
// An automatic reminder for the same version, at most this often.
constexpr auto kRemindInterval = std::chrono::days(3);

// Where the checks are remembered: next to the index, not in the settings
// file the user edits.
QSettings state()
{
    return QSettings(Settings::dataDir() + u"\\update.ini"_s, QSettings::IniFormat);
}

// Tests point this at a fake release (WINSHUN_UPDATE_FEED, an http: or https: URL).
QString feedUrl()
{
    const QString custom = qEnvironmentVariable("WINSHUN_UPDATE_FEED");
    if (!custom.isEmpty())
        return custom;
    return u"https://api.github.com/repos/%1/releases/latest"_s.arg(QLatin1StringView(Updater::kRepository));
}

} // namespace

Updater::Updater(QObject* parent)
    : QObject(parent)
{
    m_lastChecked = state().value(u"LastChecked"_s).toDateTime();
    m_timer.setInterval(1h);
    connect(&m_timer, &QTimer::timeout, this, &Updater::checkIfDue);
}

QString Updater::currentVersion() const
{
    return QCoreApplication::applicationVersion();
}

bool Updater::skipped() const
{
    return m_release && state().value(u"SkippedVersion"_s).toString() == m_release->version;
}

QString Updater::summary() const
{
    return m_release ? release::summary(m_release->notes, m_chinese) : QString();
}

QVariantList Updater::highlights() const
{
    QVariantList list;
    if (!m_release)
        return list;
    for (const release::Highlight& h : release::highlights(m_release->notes, m_chinese))
        list.append(QVariantMap {{u"symbol"_s, h.symbol}, {u"text"_s, h.text}});
    return list;
}

void Updater::setAutomatic(bool on)
{
    if (!on) {
        m_timer.stop();
        return;
    }
    if (m_timer.isActive())
        return;
    m_timer.start();
    // Right after logging in the network may not be up yet.
    QTimer::singleShot(20s, this, &Updater::checkIfDue);
}

void Updater::setChinese(bool chinese)
{
    if (m_chinese == chinese)
        return;
    m_chinese = chinese;
    emit changed();
}

void Updater::checkIfDue()
{
    if (!m_timer.isActive())
        return; // turned off meanwhile
    const bool due = !m_checkedThisRun || !m_lastChecked.isValid()
        || m_lastChecked.secsTo(QDateTime::currentDateTime()) >= std::chrono::seconds(kInterval).count();
    if (due)
        check(false);
}

void Updater::check(bool manual)
{
    if (m_checking)
        return;
    m_checking = true;
    if (manual)
        m_problem.clear();
    emit changed();

    const QString url = feedUrl();
    QThreadPool::globalInstance()->start([self = QPointer(this), url, manual] {
        const http::Response response = http::get(url,
            {{u"Accept"_s, u"application/vnd.github+json"_s}, {u"X-GitHub-Api-Version"_s, u"2022-11-28"_s}});
        QMetaObject::invokeMethod(qApp, [self, response, manual] {
            if (self)
                self->finishCheck(response.status, response.body, response.error, manual);
        }, Qt::QueuedConnection);
    });
}

void Updater::finishCheck(int status, const QByteArray& body, unsigned long error, bool manual)
{
    m_checking = false;
    const std::optional<release::Info> latest = status == 200 ? release::parse(body) : std::nullopt;
    if (!latest) {
        QString problem;
        if (status == 403 || status == 429)
            problem = tr("GitHub 暂时不让查（访问太频繁），过一会儿再试。");
        else if (status != 0 && status != 200)
            problem = tr("GitHub 返回了错误（%1）。").arg(status);
        else if (status == 200)
            problem = tr("没看懂 GitHub 的回答，过一会儿再试。");
        else if (error == ERROR_WINHTTP_TIMEOUT)
            problem = tr("连接 GitHub 超时，请检查网络（或代理）后重试。");
        else
            problem = tr("连不上 GitHub，请检查网络（或代理）后重试。");
        qWarning().noquote() << "Update check failed:" << status << error;
        if (manual)
            m_problem = problem;
        emit changed();
        return;
    }

    m_checkedThisRun = true;
    m_lastChecked = QDateTime::currentDateTime();
    state().setValue(u"LastChecked"_s, m_lastChecked);
    m_problem.clear();
    if (!release::isNewer(latest->version, currentVersion())) {
        m_release.reset();
        emit changed();
        return;
    }
    m_release = latest;
    emit changed();
    if (manual || shouldRemind(latest->version)) {
        QSettings s = state();
        s.setValue(u"PromptedVersion"_s, latest->version);
        s.setValue(u"PromptedAt"_s, QDateTime::currentDateTime());
        emit found(manual);
    }
}

// Skipped versions are not announced again; others at most every few days.
bool Updater::shouldRemind(const QString& version) const
{
    const QSettings s = state();
    if (s.value(u"SkippedVersion"_s).toString() == version)
        return false;
    const QDateTime promptedAt = s.value(u"PromptedAt"_s).toDateTime();
    return s.value(u"PromptedVersion"_s).toString() != version || !promptedAt.isValid()
        || promptedAt.secsTo(QDateTime::currentDateTime()) >= std::chrono::seconds(kRemindInterval).count();
}

void Updater::openDownloadPage()
{
    const QString page = m_release && !m_release->pageUrl.isEmpty()
        ? m_release->pageUrl
        : u"https://github.com/%1/releases/latest"_s.arg(QLatin1StringView(kRepository));
    shell::openUrl(page);
}

void Updater::skip()
{
    if (!m_release)
        return;
    state().setValue(u"SkippedVersion"_s, m_release->version);
    emit changed();
}

} // namespace ws
