// winshun.capture's program (GitHub #138): the screen capture, started by Win顺
// as a plugin. WinShunCapture.exe (stub/main.cpp) puts Win顺's folder on the
// DLL path and calls winshunCaptureMain here: Qt, ONNX Runtime and the OCR
// models are Win顺's own, nothing of them comes with the plugin.

#include "CaptureApp.h"
#include "CapturePreview.h"
#include "DiagnosticLog.h"
#include "Phosphor.h"
#include "PluginShell.h"
#include "Protocol.h"
#include "RecordingWorker.h"
#include "SystemTheme.h"
#include "WindowFrame.h"
#include "platform/WindowEffects.h"

#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonObject>
#include <QQuickImageProvider>
#include <QQuickWindow>
#include <QClipboard>
#include <QStringList>
#include <QTranslator>
#include <QLocale>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>

using namespace Qt::StringLiterals;

namespace {

// Win顺's UI font (main.cpp, applyUiFont): Alibaba PuHuiTi from its fonts\,
// read as data when the path is outside the ANSI code page (FreeType opens
// files through the C runtime).
void applyUiFont(const QString& appDir)
{
    const QString fontDir = appDir + u"/fonts/"_s;
    bool ascii = true;
    for (const QChar c : fontDir)
        ascii = ascii && c.unicode() < 128;
    QString family;
    for (const QString& file : {u"AlibabaPuHuiTi-3-55-Regular.ttf"_s}) {
        int id = -1;
        if (ascii) {
            id = QFontDatabase::addApplicationFont(fontDir + file);
        } else {
            QFile data(fontDir + file);
            if (data.open(QIODevice::ReadOnly))
                id = QFontDatabase::addApplicationFontFromData(data.readAll());
        }
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        if (family.isEmpty() && !families.isEmpty())
            family = families.first();
    }
    if (family.isEmpty()) {
        qWarning("UI font: nothing loaded from %s, using the system font", qUtf8Printable(QDir::toNativeSeparators(fontDir)));
        return;
    }
    QFont font(family);
    font.setHintingPreference(QFont::PreferVerticalHinting); // vertical only: full hinting moves glyphs sideways
    QGuiApplication::setFont(font);
}

// The program's icon in the title bars: Win顺's, read from WinShun.exe.
class AppIcon : public QQuickImageProvider {
public:
    explicit AppIcon(QString exe)
        : QQuickImageProvider(QQuickImageProvider::Image)
        , m_exe(std::move(exe))
    {
    }
    QImage requestImage(const QString&, QSize* size, const QSize& requested) override
    {
        const int side = requested.isValid() ? std::max(requested.width(), requested.height()) : 32;
        HICON icon = nullptr;
        ::SHDefExtractIconW(reinterpret_cast<LPCWSTR>(m_exe.utf16()), 0, 0, &icon, nullptr, MAKELONG(side, 0));
        QImage image;
        if (icon) {
            image = QImage::fromHICON(icon);
            ::DestroyIcon(icon);
        }
        if (size)
            *size = image.size();
        return image;
    }

private:
    const QString m_exe;
};

} // namespace

extern "C" __declspec(dllexport) int winshunCaptureWorker(const wchar_t* appDirName)
{
    const QString appDir = QString::fromWCharArray(appDirName);
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    QCoreApplication::setLibraryPaths({appDir});
    int argc = 1;
    char name[] = "WinShunCaptureWorker";
    char* argv[] = {name, nullptr};
    QGuiApplication app(argc, argv);
    applyUiFont(appDir);
    const QLocale locale(qEnvironmentVariable("WINSHUN_RECORDING_LOCALE", u"zh_CN"_s));
    QLocale::setDefault(locale);
    QTranslator translation;
    if (locale.language() == QLocale::English && translation.load(u":/i18n/capture_en.qm"_s))
        QCoreApplication::installTranslator(&translation);
    return ws::capture::recording::recordingWorkerMain();
}

extern "C" __declspec(dllexport) int winshunCaptureMain(const wchar_t* appDirName)
{
    const QString appDir = QString::fromWCharArray(appDirName);
    _setmaxstdio(8192); // FreeType's font faces, one C runtime stream each (main.cpp, raiseStreamLimit)
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    // The host's initialize first: how Win顺 draws (its renderer, its look).
    QByteArray rest;
    const std::optional<QJsonObject> first = ws::captureplugin::Protocol::readFirst(&rest);
    if (!first || first->value(u"method"_s).toString() != u"initialize") {
        std::fputs("winshun.capture: the first message was not initialize\n", stderr);
        return 2;
    }
    const QJsonObject params = first->value(u"params"_s).toObject();
    QJsonObject appearance = params.value(u"appearance"_s).toObject();
    if (!appearance.contains(u"locale"_s))
        appearance.insert(u"locale"_s, params.value(u"locale"_s).toString(u"zh"_s));
    const bool software = appearance.value(u"renderer"_s).toString() == u"software";

    QCoreApplication::setLibraryPaths({appDir}); // platforms\qwindows.dll and the rest, Win顺's
    // The font engine as Win顺 has it (main.cpp): FreeType, GDI for the software renderer.
    qputenv("QT_QPA_PLATFORM", software ? "windows:fontengine=gdi" : "windows:fontengine=freetype");
    qputenv("QT_D3D_NO_VBLANK_THREAD", "1");
    QCoreApplication::setOrganizationName(u"WinShun"_s);
    QCoreApplication::setApplicationName(u"WinShun"_s);
    int argc = 1;
    char name[] = "WinShunCapture";
    char* argv[] = {name, nullptr};
    QGuiApplication app(argc, argv);
    qunsetenv("QT_QPA_PLATFORM");
    QGuiApplication::setApplicationDisplayName(u"Win顺"_s);
    QGuiApplication::setQuitOnLastWindowClosed(false);
    QGuiApplication::clipboard()->blockSignals(true); // see main.cpp: no reading the clipboard on every change
    QQuickWindow::setGraphicsApi(software ? QSGRendererInterface::Software : QSGRendererInterface::Direct3D11);
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    ws::SystemTheme::setBackdropAvailable(!software && ws::win::backdropSupported());
    applyUiFont(appDir);
    if (ws::SystemTheme::backdropAvailable()) { // grey-scale text over Mica (App::start, GitHub #12)
        QFont font = QGuiApplication::font();
        font.setStyleStrategy(QFont::StyleStrategy(font.styleStrategy() | QFont::NoSubpixelAntialias));
        QGuiApplication::setFont(font);
    }
    ws::phosphor::loadFonts();
    ws::WindowFrame::setIcon(u"image://winshunicon/app"_s);

    const QString dataDir = params.value(u"dataDir"_s).toString(qEnvironmentVariable("WS_DATA_DIR"));
    ws::diagnostics::initialize(dataDir); // the capture's journal (capture-stall, long-capture), in its own folder

    ws::captureplugin::Protocol protocol(rest);
    ws::captureplugin::setShellProtocol(&protocol);
    ws::captureplugin::CaptureApp capture(protocol, appDir, dataDir);
    capture.engine().addImageProvider(u"winshunicon"_s, new AppIcon(QDir::toNativeSeparators(appDir + u"/WinShun.exe"_s)));
    protocol.respond(first->value(u"id"_s), QJsonObject {{u"protocolVersion"_s, u"1"_s}, {u"name"_s, u"winshun.capture"_s}});
    capture.start(appearance);
    return app.exec();
}

// A preview (CapturePreview.h): WinShunCapture.exe --preview-… with
// --app-dir <Win顺's folder>, for checking the capture without Win顺.
extern "C" __declspec(dllexport) int winshunCapturePreview(const wchar_t* appDirName)
{
    const QString appDir = QString::fromWCharArray(appDirName);
    _setmaxstdio(8192);
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    QStringList args;
    int count = 0;
    if (LPWSTR* wide = ::CommandLineToArgvW(::GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i)
            args.push_back(QString::fromWCharArray(wide[i]));
        ::LocalFree(wide);
    }
    namespace preview = ws::captureplugin::preview;
    if (!preview::wanted(args))
        return 2;
    preview::prepare(args);
    QCoreApplication::setLibraryPaths({appDir});
    QCoreApplication::setOrganizationName(u"WinShun"_s);
    QCoreApplication::setApplicationName(u"WinShun"_s);
    int argc = 1;
    char name[] = "WinShunCapture";
    char* argv[] = {name, nullptr};
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationDisplayName(u"Win顺"_s);
    preview::note(u"loading the font"_s);
    applyUiFont(appDir);
    ws::phosphor::loadFonts();
    return preview::run(args);
}
