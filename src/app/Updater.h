#pragma once

#include "Release.h"
#include "UpdatePackage.h"

#include <QDateTime>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>
#include <optional>

namespace ws {

// Checking for updates, and putting one in place. The update service
// (lcstp.com, "拾穗发布服务") answers which version is the latest for this
// Windows and package, in the format of GitHub's releases API, with the
// package's size, SHA-256 (read from GitHub by the service) and mirrors.
// The request carries the platform, the Windows version and the package
// kind, nothing personal.
//
// "Download and install": the package is downloaded from the mirrors, then
// GitHub (UpdatePackage: https only, every source checked by size and
// SHA-256), held against changes, hashed again and its version resource
// compared with the release's. The Setup is unsigned: what makes it
// trustworthy is the https answer of lcstp.com, the digest and the version,
// nothing else (see verifyPublisher). Then
// - installed: the Setup runs silently in the mode this copy was installed
//   in (for all users: it asks for administrator rights itself, so that
//   its [Run] starts Win顺 with the user's normal ones); it closes Win顺 the
//   usual way (--quit, which saves), installs, and starts the new version;
// - portable: the zip is unpacked beside the program and checked, then
//   WinShunUpdate.exe (src/updater), run from the Updates folder, waits for
//   Win顺 to quit, swaps the files (never "data"), starts the new version,
//   and rolls back when anything fails.
// The next start compares its version with the one asked for and says how
// it went (notice()). GUI thread only.
class Updater : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT FINAL)
    Q_PROPERTY(bool checking READ checking NOTIFY changed FINAL)
    Q_PROPERTY(bool available READ available NOTIFY changed FINAL) // a newer version is out
    Q_PROPERTY(QString availableVersion READ availableVersion NOTIFY changed FINAL)
    Q_PROPERTY(bool skipped READ skipped NOTIFY changed FINAL) // the user skipped that version
    // From its release notes, in the UI language (see release::summary).
    Q_PROPERTY(QString summary READ summary NOTIFY changed FINAL)
    Q_PROPERTY(QVariantList highlights READ highlights NOTIFY changed FINAL) // [{symbol, text}]
    Q_PROPERTY(QString problem READ problem NOTIFY changed FINAL) // why the last manual check failed
    Q_PROPERTY(QDateTime lastChecked READ lastChecked NOTIFY changed FINAL)
    Q_PROPERTY(bool portable READ portable CONSTANT FINAL) // "download and update" rather than "install"
    // "", "downloading", "verifying", "installing" (Setup runs, or Win顺 is about to quit)
    Q_PROPERTY(QString installStage READ installStage NOTIFY installChanged FINAL)
    Q_PROPERTY(double installProgress READ installProgress NOTIFY installChanged FINAL) // 0..1 while downloading
    Q_PROPERTY(double downloadedMB READ downloadedMB NOTIFY installChanged FINAL)
    Q_PROPERTY(double packageMB READ packageMB NOTIFY installChanged FINAL)
    Q_PROPERTY(QString installProblem READ installProblem NOTIFY installChanged FINAL) // why the last attempt failed

public:
    explicit Updater(QObject* parent = nullptr);
    ~Updater() override;

    QString currentVersion() const;
    bool checking() const { return m_checking; }
    bool available() const { return m_release.has_value(); }
    QString availableVersion() const { return m_release ? m_release->version : QString(); }
    bool skipped() const;
    QString summary() const;
    QVariantList highlights() const;
    QString problem() const { return m_problem; }
    QDateTime lastChecked() const { return m_lastChecked; }
    bool portable() const { return m_kind == update::Kind::Portable; }
    QString installStage() const;
    double installProgress() const;
    double downloadedMB() const { return m_received / 1048576.0; }
    double packageMB() const { return m_total / 1048576.0; }
    QString installProblem() const { return m_installProblem; }

    void setAutomatic(bool on); // at startup and when the setting changes
    // At start, once notice() is connected: how the last update went, and
    // what it left removed. (Not for a preview's Updater.)
    void afterStart();
    void setChinese(bool chinese); // which half of the release notes to show

    // `manual`: the user asked, so a failure is reported and a newer version
    // is always shown (found()), even a skipped one.
    Q_INVOKABLE void check(bool manual);
    Q_INVOKABLE void openDownloadPage(); // the release's page on lcstp.com (never another site)
    Q_INVOKABLE void skip(); // no more reminders for this version; manual checks still show it
    Q_INVOKABLE void install(); // download, check and install the newer version
    Q_INVOKABLE void cancelInstall(); // while downloading

signals:
    void changed();
    void installChanged();
    // A newer version: asked for, or an automatic check that should remind.
    void found(bool manual);
    // After an update, at the next start: how it went (a tray notification).
    void notice(const QString& title, const QString& text);

private:
    enum class Stage { Idle, Downloading, Verifying, Installing };
    struct Job; // a download and check under way

    void checkIfDue();
    void finishCheck(int status, const QByteArray& body, unsigned long error, bool manual);
    bool shouldRemind(const QString& version) const;
    void setStage(Stage stage);
    void fail(const QString& problem);
    void downloaded(const std::shared_ptr<Job>& job, bool ok, bool cancelled, const QString& why);
    void verified(const std::shared_ptr<Job>& job, const QString& why);
    void runSetup(const std::shared_ptr<Job>& job);
    void runPortableSwap(const std::shared_ptr<Job>& job);
    void setupEnded(unsigned long exitCode);
    void reportLastUpdate(); // at start
    void cleanUp(); // what earlier updates left
    void setPending(const QString& version);

    std::optional<release::Info> m_release; // newer than this one
    update::Kind m_kind = update::Kind::None;
    bool m_checking = false;
    bool m_checkedThisRun = false;
    bool m_chinese = true;
    QString m_problem;
    QDateTime m_lastChecked;
    QTimer m_timer;
    Stage m_stage = Stage::Idle;
    qint64 m_received = 0;
    qint64 m_total = 0;
    QString m_installProblem;
    std::shared_ptr<Job> m_job;
    QTimer m_progressTimer; // the download's bytes, shown a few times a second
};

} // namespace ws
