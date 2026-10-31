#include "Onboarding.h"

#include "Host.h"
#include "PluginGallery.h"
#include "PluginSettings.h"
#include "platform/HotkeyText.h"
#include "platform/ShortcutCapture.h"

#include <QCoreApplication>
#include <QDir>
#include <QKeyEvent>
#include <QSettings>
#include <QTimer>
#include <QWindow>

namespace ws {
using namespace Qt::StringLiterals;

namespace {
const QString kCapture = u"winshun.capture"_s;

UINT virtualKey(const QKeyEvent& event)
{
    if (event.nativeVirtualKey())
        return event.nativeVirtualKey();
    switch (event.key()) {
    case Qt::Key_Control: return VK_CONTROL;
    case Qt::Key_Alt: return VK_MENU;
    case Qt::Key_Shift: return VK_SHIFT;
    case Qt::Key_Meta: return VK_LWIN;
    case Qt::Key_Space: return VK_SPACE;
    case Qt::Key_QuoteLeft: return VK_OEM_3;
    default:
        if (event.key() >= Qt::Key_F1 && event.key() <= Qt::Key_F24)
            return VK_F1 + event.key() - Qt::Key_F1;
        return event.key() < 128 ? event.key() : 0;
    }
}

bool matches(const QKeyEvent& event, const QString& shortcut)
{
    UINT modifiers = 0, key = 0;
    if (!hotkeys::parse(shortcut, &modifiers, &key) || key != virtualKey(event))
        return false;
    UINT actual = MOD_NOREPEAT;
    if (event.modifiers().testFlag(Qt::ControlModifier)) actual |= MOD_CONTROL;
    if (event.modifiers().testFlag(Qt::AltModifier)) actual |= MOD_ALT;
    if (event.modifiers().testFlag(Qt::ShiftModifier)) actual |= MOD_SHIFT;
    if (event.modifiers().testFlag(Qt::MetaModifier)) actual |= MOD_WIN;
    return modifiers == actual;
}

bool available(const QString& shortcut)
{
    UINT modifiers = 0, key = 0;
    if (!hotkeys::allowed(shortcut) || !hotkeys::parse(shortcut, &modifiers, &key))
        return false;
    constexpr int id = 0x7350;
    if (!::RegisterHotKey(nullptr, id, modifiers, key))
        return false;
    ::UnregisterHotKey(nullptr, id);
    return true;
}
} // namespace

Onboarding::Onboarding(const Settings& settings, QObject* parent)
    : QObject(parent), m_settings(settings)
{
    // New users explicitly opt into recording and background content work.
    connect(this, &Onboarding::choicesChanged, this, [this] {
        stopTest();
        m_testMessage.clear();
        m_message.clear();
        emit stateChanged();
    });
}

Onboarding::~Onboarding() = default;

void Onboarding::setWindow(QWindow* window)
{
    m_window = window;
    window->installEventFilter(this);
    connect(window, &QWindow::activeChanged, this, [this] {
        if (!m_window || !m_window->isActive()) {
            stopTest();
            setRecording(false);
        }
    });
}

QString Onboarding::validateOpening() const
{
    if (m_activation < 0 || m_activation > 3)
        return tr("请选择打开 Win顺 的方式。");
    if (m_activation == 3 && !available(m_launchHotkey))
        return tr("打开 Win顺 的快捷键无效或已被占用，请换一个。");
    return {};
}

QString Onboarding::validate()
{
    if (const QString problem = validateOpening(); !problem.isEmpty()) {
        m_page = 0;
        return problem;
    }
    m_page = 1;
    if (m_captureEnabled && !available(m_captureHotkey))
        return tr("请为截图录制一个可用的快捷键。");
    const QString launch = m_activation == 3 ? hotkeys::normalized(m_launchHotkey) : QString();
    const QString capture = m_captureEnabled ? hotkeys::normalized(m_captureHotkey) : QString();
    if (!launch.isEmpty() && launch == capture)
        return tr("搜索和截图需要使用不同的快捷键。");
    if (m_clipboard && m_clipboardWinV && (launch == u"win+v" || capture == u"win+v"))
        return tr("Win+V 已用于剪贴板历史，请换一个快捷键。");
    return {};
}

void Onboarding::next()
{
    if (m_committed || m_page != 0)
        return;
    stopTest();
    setRecording(false);
    m_testMessage.clear();
    m_message = validateOpening();
    if (m_message.isEmpty())
        m_page = 1;
    emit stateChanged();
}

void Onboarding::previous()
{
    if (m_committed || m_page != 1)
        return;
    stopTest();
    setRecording(false);
    m_testMessage.clear();
    m_message.clear();
    m_page = 0;
    emit stateChanged();
}

void Onboarding::apply(bool later)
{
    if (m_committed)
        return;
    stopTest();
    setRecording(false);
    m_message = later ? QString() : validate();
    if (!m_message.isEmpty()) {
        emit stateChanged();
        return;
    }
    Settings chosen = m_settings;
    chosen.onboardingComplete = true;
    chosen.onboardingCapturePending = !later && m_captureEnabled;
    chosen.contentIndex = !later && m_contentIndex;
    chosen.clipboard = !later && m_clipboard;
    chosen.clipboardWinV = chosen.clipboard && m_clipboardWinV;
    chosen.doubleCtrl = later || m_activation != 3;
    chosen.doubleTapKey = later || m_activation == 0 ? u"ctrl"_s : m_activation == 1 ? u"alt"_s : u"shift"_s;
    chosen.hotkey = !later && m_activation == 3 ? m_launchHotkey : QString();
    if (!chosen.saveChecked()) {
        m_message = tr("无法保存设置，请检查配置目录是否可写后重试。你的选择仍保留在此页面。");
        emit stateChanged();
        return;
    }
    m_settings = std::move(chosen);
    m_captureEnabled = m_settings.onboardingCapturePending;
    m_committed = true;
    m_message = tr("正在准备 Win顺…");
    emit stateChanged();
    emit accepted();
}

void Onboarding::setRecording(bool on)
{
    if (m_recording == on)
        return;
    m_recording = on;
    if (on)
        stopTest();
    updateKeyCapture();
    emit recordingChanged();
}

void Onboarding::updateKeyCapture()
{
    m_keyCapture.reset();
    if ((!m_recording && !m_testing) || !m_window || !m_window->isActive())
        return;
    m_keyCapture = std::make_unique<ShortcutCapture>(reinterpret_cast<HWND>(m_window->winId()),
        [this](const ShortcutCapture::Key& key) {
            QMetaObject::invokeMethod(this, [this, key] {
                if ((!m_recording && !m_testing) || !m_window || !m_window->isActive())
                    return;
                QKeyEvent event(key.press ? QEvent::KeyPress : QEvent::KeyRelease, key.key, key.modifiers,
                    QString(), key.autoRepeat);
                QCoreApplication::sendEvent(m_window, &event);
            }, Qt::QueuedConnection);
        });
    if (!m_keyCapture->isActive())
        m_keyCapture.reset();
}

void Onboarding::beginTest(bool capture)
{
    if (m_committed)
        return;
    setRecording(false);
    m_testCapture = capture;
    const QString shortcut = capture ? m_captureHotkey : m_activation == 3 ? m_launchHotkey : QString();
    if ((capture || m_activation == 3) && !hotkeys::allowed(shortcut)) {
        m_testMessage = tr("请先录制快捷键。");
        emit stateChanged();
        return;
    }
    DoubleTapDetector::Config config;
    config.key = m_activation == 1 ? DoubleTapDetector::Key::Alt : m_activation == 2 ? DoubleTapDetector::Key::Shift
                                                                                  : DoubleTapDetector::Key::Ctrl;
    m_doubleTap = DoubleTapDetector(config);
    m_testing = true;
    m_testMessage = tr("现在按下选定的按键试试。Esc 取消；仅在此窗口内测试。");
    updateKeyCapture();
    emit stateChanged();
}

void Onboarding::stopTest()
{
    if (!m_testing)
        return;
    m_testing = false;
    m_testMessage.clear();
    updateKeyCapture();
    emit stateChanged();
}

bool Onboarding::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != m_window || !m_testing)
        return false;
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease) {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::Wheel)
            m_doubleTap.mouseUsed();
        return false;
    }
    const auto& key = *static_cast<QKeyEvent*>(event);
    if (key.isAutoRepeat())
        return true;
    if (key.key() == Qt::Key_Escape) {
        stopTest();
        return true;
    }
    const bool pressed = event->type() == QEvent::KeyPress;
    const bool matched = m_testCapture || m_activation == 3
        ? pressed && matches(key, m_testCapture ? m_captureHotkey : m_launchHotkey)
        : pressed ? m_doubleTap.keyDown(virtualKey(key), ::GetTickCount(), 0, 0)
                  : m_doubleTap.keyUp(virtualKey(key), ::GetTickCount(), 0, 0);
    if (matched) {
        stopTest();
        m_testMessage = tr("识别成功，开始使用后即可生效。");
        emit stateChanged();
    }
    return true;
}

void Onboarding::attachPlugins(PluginSettings* plugins, plugin::Host* host)
{
    m_plugins = plugins;
    m_host = host;
    m_captureFolder = QDir(host->options().dataRoot).filePath(kCapture + u".data");
    emit pluginsChanged();
    connect(plugins, &PluginSettings::listChanged, this, &Onboarding::updateCaptureState);
    connect(plugins, &PluginSettings::stateChanged, this, &Onboarding::updateCaptureState);
    connect(host, &plugin::Host::stateChanged, this, [this](const QString& id) {
        if (id == kCapture)
            updateCaptureState();
    });
    connect(plugins, &PluginSettings::messageChanged, this, [this] {
        QTimer::singleShot(0, this, &Onboarding::updateCaptureState);
    });
    connect(plugins, &PluginSettings::confirmationNeeded, this, [this](const QVariantMap& info) {
        if (info.value(u"id"_s).toString() == kCapture) {
            m_waitingPermission = true;
            m_enableRequested = true;
            m_busy = false;
            m_message = tr("请确认截图组件所需的权限。");
            emit stateChanged();
        }
    });
    auto* gallery = plugins->gallery();
    connect(gallery, &PluginGallery::stateChanged, this, &Onboarding::updateCaptureState);
    connect(gallery, &PluginGallery::listChanged, this, &Onboarding::updateCaptureState);
    // Allow QML to create the existing permission dialog before any request.
    QTimer::singleShot(0, this, &Onboarding::retryCapture);
}

void Onboarding::captureProblem(const QString& message)
{
    m_busy = false;
    m_canRetry = true;
    m_progress = -1;
    m_message = message;
    emit stateChanged();
}

void Onboarding::retryCapture()
{
    if (!m_plugins || m_captureReady)
        return;
    if (!QDir().mkpath(m_captureFolder)) {
        captureProblem(tr("无法创建截图配置目录。可稍后在“设置 → 插件”中安装。"));
        return;
    }
    QSettings capture(QDir(m_captureFolder).filePath(u"capture.ini"_s), QSettings::IniFormat);
    capture.setValue(u"Capture/Hotkey"_s, m_captureHotkey);
    capture.setValue(u"Capture/PrintScreen"_s, false);
    capture.sync();
    if (capture.status() != QSettings::NoError) {
        captureProblem(tr("无法保存截图快捷键。可稍后在“设置 → 插件”中重试。"));
        return;
    }
    m_canRetry = false;
    m_busy = true;
    m_waitingPermission = false;
    m_installRequested = false;
    m_enableRequested = false;
    m_plugins->clearMessage();
    m_message = tr("正在准备截图组件…");
    emit stateChanged();
    if (const auto* card = m_plugins->list()->card(kCapture);
        card && card->enabled && (card->tone == u"error" || card->tone == u"warn"))
        m_host->reload(kCapture);
    if (const auto* card = m_plugins->list()->card(kCapture); !card || !card->usable)
        m_plugins->gallery()->refresh();
    updateCaptureState();
}

void Onboarding::updateCaptureState()
{
    if (!m_plugins || m_captureReady || m_waitingPermission || m_canRetry)
        return;
    const auto* card = m_plugins->list()->card(kCapture);
    if (card && card->usable) {
        if (card->enabled) {
            if (card->tone == u"error" || card->tone == u"warn") {
                captureProblem(card->error.isEmpty() ? card->status : card->error);
                return;
            }
            if (card->tone == u"ok" && m_host->state(kCapture).status == plugin::Host::State::Status::Running) {
                m_captureReady = true;
                m_busy = false;
                m_progress = 1;
                m_message = tr("截图已启用，按 %1 即可框选、标注或贴图。").arg(m_captureHotkey);
                emit stateChanged();
                emit captureActivated();
                return;
            }
            m_message = tr("正在启动截图组件…");
        } else if (!m_enableRequested && !m_installRequested) {
            m_enableRequested = true; // setEnabled may synchronously ask for consent
            m_plugins->setEnabled(kCapture, true);
            return;
        }
    } else {
        auto* gallery = m_plugins->gallery();
        if (gallery->fetching()) {
            m_message = tr("正在获取截图组件信息…");
        } else {
            const auto& rows = gallery->list()->rows();
            const auto row = std::ranges::find(rows, kCapture, &PluginGalleryModel::Row::id);
            if (row == rows.end()) {
                captureProblem(gallery->problem().isEmpty() ? tr("暂时找不到截图组件，请稍后重试。") : gallery->problem());
                return;
            }
            if (row->stage == u"app") {
                captureProblem(tr("此截图组件需要 Win顺 %1 或更新版本。可稍后在插件库中安装。").arg(row->minWinShun));
                return;
            }
            if (!row->problem.isEmpty()) {
                captureProblem(row->problem);
                return;
            }
            if (!m_installRequested) {
                if (row->stage != u"install" && row->stage != u"update") {
                    captureProblem(card && !card->error.isEmpty() ? card->error
                        : tr("截图组件暂时无法使用，请在“设置 → 插件”中检查或重新安装。"));
                    return;
                }
                m_installRequested = true;
                gallery->installAndEnable(kCapture);
                return;
            }
            m_progress = row->busy ? row->progress : -1;
            m_message = row->busy ? tr("正在下载截图组件… %1%").arg(qRound(row->progress * 100))
                                 : tr("正在安装截图组件…");
        }
    }
    if (!card && m_installRequested && !m_plugins->gallery()->downloading()
        && !m_plugins->message().isEmpty() && !m_waitingPermission) {
        captureProblem(m_plugins->message());
        return;
    }
    emit stateChanged();
}

void Onboarding::permissionClosed()
{
    QTimer::singleShot(0, this, [this] {
        if (!m_waitingPermission)
            return;
        m_waitingPermission = false;
        const auto* card = m_plugins->list()->card(kCapture);
        if (!card || !card->enabled)
            captureProblem(tr("截图尚未启用。可以重试，或稍后在“设置 → 插件”中开启。"));
        else {
            m_busy = true;
            updateCaptureState();
        }
    });
}

void Onboarding::finish()
{
    if (!m_committed)
        return;
    if (m_plugins) {
        m_plugins->gallery()->cancel(kCapture);
        m_plugins->cancelInstall();
    }
    emit finished();
}

} // namespace ws
