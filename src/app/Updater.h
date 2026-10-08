#pragma once

#include "Release.h"

#include <QDateTime>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <optional>

namespace ws {

// Checking for updates. There is no server of our own: new versions are
// GitHub releases (scripts/release.ps1 -Publish), and this asks GitHub's
// public API which one is the latest. A newer one is announced; "download"
// opens its release page in the browser, and the user unzips it over the old
// folder. Nothing is downloaded or replaced here: an unsigned program that
// fetches executables and rewrites its own files is what antivirus
// heuristics flag, and this one runs as administrator.
// The request carries nothing personal. GUI thread only.
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

public:
    static constexpr auto kRepository = "LingCore/WinShun";

    explicit Updater(QObject* parent = nullptr);

    QString currentVersion() const;
    bool checking() const { return m_checking; }
    bool available() const { return m_release.has_value(); }
    QString availableVersion() const { return m_release ? m_release->version : QString(); }
    bool skipped() const;
    QString summary() const;
    QVariantList highlights() const;
    QString problem() const { return m_problem; }
    QDateTime lastChecked() const { return m_lastChecked; }

    void setAutomatic(bool on); // at startup and when the setting changes
    void setChinese(bool chinese); // which half of the release notes to show

    // `manual`: the user asked, so a failure is reported and a newer version
    // is always shown (found()), even a skipped one.
    Q_INVOKABLE void check(bool manual);
    Q_INVOKABLE void openDownloadPage(); // the new version's release page (or the latest)
    Q_INVOKABLE void skip(); // no more reminders for this version; manual checks still show it

signals:
    void changed();
    // A newer version: asked for, or an automatic check that should remind.
    void found(bool manual);

private:
    void checkIfDue();
    void finishCheck(int status, const QByteArray& body, unsigned long error, bool manual);
    bool shouldRemind(const QString& version) const;

    std::optional<release::Info> m_release; // newer than this one
    bool m_checking = false;
    bool m_checkedThisRun = false;
    bool m_chinese = true;
    QString m_problem;
    QDateTime m_lastChecked;
    QTimer m_timer;
};

} // namespace ws
