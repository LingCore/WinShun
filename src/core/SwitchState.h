#pragma once

#include <QDateTime>
#include <QString>
#include <QtGlobal>

namespace ws {

// Plain state shared by the search UI and the optional switch provider.
struct SwitchState {
    enum class Availability : quint8 { Unknown, Ready, NoDevice, Denied, Blocked, Unsupported };
    enum class Value : qint8 { Unknown = -1, Off = 0, On = 1, Mixed = 2 };
    Availability availability = Availability::Unknown;
    Value on = Value::Unknown;
    qint8 level = -1;
    bool busy = false;
    QString note;
    QString error;
    QDateTime until;
    QString openInstead;
    bool mixedToggleOn = false;
    bool carriesInternet = false;

    [[nodiscard]] bool ready() const noexcept { return availability == Availability::Ready; }
    friend bool operator==(const SwitchState&, const SwitchState&) = default;
};

struct SwitchWrite {
    bool ok = false;
    bool wrote = false;
    QString error;
    SwitchState after;
};

} // namespace ws
