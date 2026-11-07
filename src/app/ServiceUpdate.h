#pragma once

// When a portable (or Scoop) copy offers to update the index service it
// installed. Such a copy registers the service from a copy of its
// WinShunService.exe in %ProgramFiles%\WinShun Service (ServiceLocation.h);
// updating Win顺 itself (in-app update, a new zip, scoop update) leaves that
// copy as it was, so features that live in the service (the key helper's
// relayed hotkeys, GitHub #243) would wait for a reinstall nobody knows of.
// `WinShunService.exe --install` from the new copy upgrades it in place (UAC
// once). Pure, for tst_serviceupdate; App gathers the facts.

#include <QString>
#include <QStringList>

#include <array>
#include <optional>

namespace ws::serviceupdate {

// "0.22.0" -> {0, 22, 0}; anything else (empty, "0.22", "0.22.0-beta") none.
inline std::optional<std::array<int, 3>> parseVersion(const QString& text)
{
    const QStringList parts = text.split(u'.');
    if (parts.size() != 3)
        return std::nullopt;
    std::array<int, 3> version {};
    for (int i = 0; i < 3; ++i) {
        bool ok = false;
        version[static_cast<std::size_t>(i)] = parts[i].toInt(&ok);
        if (!ok || version[static_cast<std::size_t>(i)] < 0)
            return std::nullopt;
    }
    return version;
}

struct Facts {
    bool portable = false; // a portable or Scoop copy: the installer updates an installed one's service
    bool installable = false; // the account can install it, and WinShunService.exe is beside this Win顺
    bool ownCopy = false; // the service's --installed-from is this very copy (never the installer's, never another's)
    QString serviceVersion; // the running service's, from its handshake; empty when it has not answered
    QString appVersion; // WINSHUN_VERSION
};

// Offered only for this copy's own service, and only to bring it up to this
// copy's version: never a downgrade, never another copy's service (running
// --install from here would point the one "WinShun" service at this folder).
inline bool due(const Facts& f)
{
    if (!f.portable || !f.installable || !f.ownCopy)
        return false;
    const auto service = parseVersion(f.serviceVersion);
    const auto app = parseVersion(f.appVersion);
    return service && app && *service < *app;
}

} // namespace ws::serviceupdate
