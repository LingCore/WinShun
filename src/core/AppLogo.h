#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace ws {

// The logo a packaged (Store / MSIX) app shows in Start and on the taskbar,
// picked the way Windows (and PowerToys Command Palette) picks it: from the
// Square44x44Logo the app's AppxManifest.xml names, the file drawn for this
// many pixels ("targetsize-48") in the variant for this theme ("altform-
// unplated" on dark, "altform-lightunplated" on light). Assets drawn for an
// exact size stay sharp; scaling another size never quite does.
struct AppLogo {
    QString path;
    bool plated = false; // made to sit on `background`, not on any backdrop
    QString background; // the manifest's BackgroundColor: "#RRGGBB", a color name, or "transparent"
};

// Caller has COM initialised. Nullopt for desktop programs, and when the
// package or its files cannot be read.
std::optional<AppLogo> findAppLogo(const QString& appId, int pixels, bool dark);

struct LogoFile {
    QString name; // as given in `files`
    bool plated = false;
};

// The best of `files` (file names or paths) for the logical asset name
// `logical` ("Square44x44Logo.png"). `resolved` is the file Windows' resource
// system picked for the current language and scale, if any: files for another
// language or configuration are left out.
std::optional<LogoFile> chooseLogoFile(
    const QString& logical, const QStringList& files, int pixels, bool dark, const QString& resolved = {});

} // namespace ws
