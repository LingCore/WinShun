#include "ClipboardDisplayReplica.h"
#include "UiFont.h"
#include <QFont>
#include <cstdio>

#include "Clipboard.h"
#include "ClipThumbnailProvider.h"
#include "Placement.h"
#include "WindowFrame.h"
#include "SystemTheme.h"
#include "ContextMenuGuard.h"
#include "TextField.h"
#include "ClipboardDisplay.h"
#include "platform/WindowEffects.h"

#include <QElapsedTimer>
#include <QClipboard>
#include <QBuffer>
#include <QImageReader>
#include <QMetaMethod>
#include <QLocale>
#include <QGuiApplication>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMetaProperty>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTranslator>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace ws {

DisplayRows::DisplayRows(QObject* parent) : QAbstractListModel(parent)
{
    const QList<QByteArray> names {"clipId", "kind", "title", "detail", "icon", "image", "swatch", "order",
        "groupName", "pinned", "missing", "movable", "afterPinned", "screenshot", "pasted"};
    for (int i = 0; i < names.size(); ++i) m_roles.insert(Qt::UserRole + i + 1, names[i]);
}

int DisplayRows::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : count(); }

QVariant DisplayRows::data(const QModelIndex& index, int role) const
{
    return index.row() >= 0 && index.row() < count()
        ? m_rows[index.row()].value(QString::fromLatin1(m_roles.value(role))).toVariant() : QVariant();
}

qint64 DisplayRows::idAt(int row) const
{
    return row >= 0 && row < count() ? m_rows[row].value(u"clipId"_s).toInteger() : 0;
}

bool DisplayRows::isSelected(int row) const
{
    return row >= 0 && row < count() && m_rows[row].value(u"order"_s).toInt() > 0;
}

bool DisplayRows::begin(int count, int selected)
{
    if (count < 0 || count > 100'000 || selected < 0 || selected > count)
        return false;
    m_expected = count;
    m_pendingSelected = selected;
    m_pending.clear();
    m_pending.reserve(count);
    return true;
}

bool DisplayRows::append(int from, const QJsonArray& rows)
{
    if (m_expected < 0 || from != m_pending.size() || from + rows.size() > m_expected)
        return false;
    for (const auto& row : rows) {
        if (!row.isObject()) return false;
        m_pending.push_back(row.toObject());
    }
    return true;
}

bool DisplayRows::commit()
{
    if (m_expected < 0 || m_pending.size() != m_expected)
        return false;
    bool sameIds = m_rows.size() == m_pending.size();
    if (sameIds)
        for (int i = 0; i < m_rows.size(); ++i)
            sameIds &= m_rows[i].value(u"clipId"_s) == m_pending[i].value(u"clipId"_s);
    const int oldCount = count();
    if (sameIds) {
        for (int i = 0; i < m_rows.size(); ++i) {
            if (m_rows[i] == m_pending[i]) continue;
            m_rows[i] = std::move(m_pending[i]);
            emit dataChanged(index(i), index(i));
        }
    } else {
        beginResetModel();
        m_rows = std::move(m_pending);
        endResetModel();
    }
    m_pending.clear();
    m_expected = -1;
    if (oldCount != count()) emit countChanged();
    if (m_selected != m_pendingSelected) {
        m_selected = m_pendingSelected;
        emit selectionChanged();
    }
    return true;
}

QQuickTextureFactory* DisplayImage::textureFactory() const { return QQuickTextureFactory::textureFactoryForImage(m_image); }
void DisplayImage::finish(QImage image) { m_image = std::move(image); emit finished(); }

DisplayReplica::DisplayReplica(QQmlEngine* engine) : QQmlPropertyMap(this, nullptr), m_engine(engine)
{
    const QMetaObject& meta = Clipboard::staticMetaObject;
    for (int i = meta.propertyOffset(); i < meta.propertyCount(); ++i) {
        const auto property = meta.property(i);
        if (QByteArrayView(property.name()) == "items" || QByteArrayView(property.name()) == "field") continue;
        insert(QString::fromLatin1(property.name()), QVariant(property.metaType()));
    }
    insert(u"listWidth"_s, 480);
    insert(u"numberKeys"_s, u"alt"_s);
    insert(u"numberHints"_s, u"auto"_s);
    insert(u"items"_s, QVariant::fromValue<QObject*>(&m_rows));
    insert(u"field"_s, QVariant::fromValue<QObject*>(nullptr));
    // Callable values keep the existing QML interface; only the explicitly
    // allowed controller methods are accepted by the ordinary-process peer.
    const QJSValue bridge = engine->newQObject(this);
    for (int i = meta.methodOffset(); i < meta.methodCount(); ++i) {
        const auto method = meta.method(i);
        if (method.methodType() == QMetaMethod::Signal && method.name() != "groupNameRequested") continue;
        const QString name = QString::fromLatin1(method.name());
        if (name == u"groupNameRequested") continue; // its signal is a callable QObject method
        const QString source = u"(function(b){return function(){return b.invoke('%1',Array.prototype.slice.call(arguments));};})"_s.arg(name);
        const QJSValue factory = engine->evaluate(source);
        insert(name, QVariant::fromValue(factory.call({bridge})));
    }
    connect(this, &QQmlPropertyMap::valueChanged, this, [this](const QString& name, const QVariant& value) {
        if (!m_applying) send({{u"type"_s, u"set"_s}, {u"name"_s, name}, {u"value"_s, QJsonValue::fromVariant(value)}});
    });
    connect(&m_rows, &DisplayRows::highlightColorChanged, this, [this] {
        send({{u"type"_s, u"highlight"_s}, {u"color"_s, m_rows.property("highlightColor").value<QColor>().name(QColor::HexArgb)},
            {u"weight"_s, m_rows.property("highlightWeight").toInt()}});
    });
    connect(this, &DisplayReplica::groupNameRequested, this, [this](int row, qint64 group) {
        if (!m_applying) invoke(u"groupNameRequested"_s, {row, group});
    });
    connect(&m_socket, &QLocalSocket::readyRead, this, &DisplayReplica::receive);
    connect(&m_socket, &QLocalSocket::disconnected, this, &DisplayReplica::failed);
    m_watch.setInterval(1000);
    connect(&m_watch, &QTimer::timeout, this, [this] {
        if (!m_parent || ::WaitForSingleObject(m_parent, 0) != WAIT_TIMEOUT) failed();
    });
}

DisplayReplica::~DisplayReplica() { if (m_parent) ::CloseHandle(m_parent); }

bool DisplayReplica::connectTo(const QString& pipe, DWORD parentPid)
{
    m_parentPid = parentPid;
    m_socket.setReadBufferSize(2 * clipdisplay::kMaxFrame);
    m_parent = ::OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (!m_parent) return false;
    m_socket.connectToServer(pipe);
    if (!m_socket.waitForConnected(3000)) return false;
    ULONG actual = 0;
    if (!::GetNamedPipeServerProcessId(reinterpret_cast<HANDLE>(m_socket.socketDescriptor()), &actual) || actual != parentPid)
        return false;
    QElapsedTimer deadline;
    deadline.start();
    while (!m_failed && !m_fontReady && deadline.elapsed() < 3000) {
        receive();
        if (!m_fontReady)
            m_socket.waitForReadyRead(std::max(1, 3000 - int(deadline.elapsed())));
    }
    if (m_failed || !m_fontReady)
        return false;
    m_watch.start();
    return true;
}

void DisplayReplica::setWindow(QQuickWindow* window, Placement* placement, WindowFrame* frame)
{
    m_window = window;
    m_placement = placement;
    m_frame = frame;
    connect(window, &QWindow::visibleChanged, this, [this](bool visible) {
        if (!visible && !m_applying && !m_failed) send({{u"type"_s, u"closed"_s}});
    });
    connect(window, &QWindow::activeChanged, this, [this] {
        if (!m_window->isActive()) QTimer::singleShot(0, this, [this] {
            if (m_window && m_window->isVisible() && !m_window->isActive() && !m_failed)
                send({{u"type"_s, u"focusLost"_s}});
        });
    });
    connect(placement, &Placement::movingChanged, this, [this] {
        if (!m_placement->moving()) {
            const QRect box = m_window->geometry();
            send({{u"type"_s, u"geometry"_s}, {u"box"_s, QJsonArray {box.x(), box.y(), box.width(), box.height()}}});
        }
    });
}

void DisplayReplica::announce(bool uiAccess, DWORD band)
{
    send({{u"type"_s, u"ready"_s}, {u"uiAccess"_s, uiAccess}, {u"band"_s, int(band)},
        {u"window"_s, QString::number(m_window->winId())}});
}

void DisplayReplica::send(const QJsonObject& object)
{
    const QByteArray bytes = clipdisplay::frame(object);
    if (bytes.isEmpty() || m_socket.state() != QLocalSocket::ConnectedState || m_socket.bytesToWrite() > 4 * clipdisplay::kMaxFrame) {
        failed();
        return;
    }
    m_socket.write(bytes);
    m_socket.flush();
}

void DisplayReplica::failed()
{
    if (m_failed) return;
    m_failed = true;
    if (m_window) m_window->hide();
    m_socket.abort();
    QCoreApplication::exit(4); // a disconnected display never keeps taking input
}

QVariant DisplayReplica::updateValue(const QString&, const QVariant& input) { return input; }

QVariant DisplayReplica::invoke(const QString& name, const QVariantList& arguments)
{
    if (name == u"prepareMenuWindow") {
        auto* menu = arguments.value(0).value<QWindow*>();
        if (menu) {
            menu->setProperty("_q_showWithoutActivating", true);
            win::styleFramelessWindow(menu);
            win::setDarkFrame(menu, QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
        }
        return {};
    }
    if (name == u"screenArea") {
        QScreen* screen = QGuiApplication::screenAt(arguments.value(0).toPointF().toPoint());
        if (!screen && m_window) screen = m_window->screen();
        return screen ? QRectF(screen->availableGeometry()) : QRectF();
    }
    static const QStringList returnsValue {u"heldRow"_s, u"canMove"_s, u"addGroup"_s, u"selectAll"_s,
        u"preview"_s, u"menuItems"_s, u"categoryMenuItems"_s};
    if (!returnsValue.contains(name)) {
        send({{u"type"_s, u"call"_s}, {u"id"_s, 0}, {u"name"_s, name}, {u"args"_s, QJsonArray::fromVariantList(arguments)}});
        return {}; // drag/drop and native pickers can keep their ordinary-process modal loop
    }
    const qint64 id = ++m_serial;
    send({{u"type"_s, u"call"_s}, {u"id"_s, id}, {u"name"_s, name}, {u"args"_s, QJsonArray::fromVariantList(arguments)}});
    QElapsedTimer deadline;
    deadline.start();
    while (!m_failed && !m_replies.contains(id) && deadline.elapsed() < 500) {
        receive();
        if (!m_replies.contains(id)) m_socket.waitForReadyRead(std::max(1, 500 - int(deadline.elapsed())));
    }
    if (!m_replies.contains(id)) { failed(); return {}; }
    return m_replies.take(id);
}

void DisplayReplica::requestImage(const QString& key, QSize size, DisplayImage* response)
{
    const qint64 id = ++m_serial;
    m_images.insert(id, response);
    send({{u"type"_s, u"image"_s}, {u"id"_s, id}, {u"key"_s, key}, {u"width"_s, size.width()}, {u"height"_s, size.height()}});
}

void DisplayReplica::receive()
{
    if (!m_decoder.append(m_socket.readAll())) { failed(); return; }
    QJsonObject object;
    while (!m_failed && m_decoder.take(object)) dispatch(object);
    if (m_decoder.failed()) failed();
}

void DisplayReplica::applyState(const QJsonObject& object)
{
    m_applying = true;
    QGuiApplication::styleHints()->setColorScheme(object.value(u"dark"_s).toBool() ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    SystemTheme::setUiSize(object.value(u"uiSize"_s).toString());
    SystemTheme::setRoundedCorners(object.value(u"rounded"_s).toBool(true));
    SystemTheme::setUiStyle(object.value(u"uiStyle"_s).toString());
    SystemTheme::setBackdropOn(object.value(u"backdrop"_s).toBool());
    const QString language = object.value(u"language"_s).toString();
    if (m_language != language) {
        m_language = language;
        QCoreApplication::removeTranslator(&m_translator);
        if (language.startsWith(u"en") && m_translator.load(u":/i18n/winshun_en.qm"_s))
            QCoreApplication::installTranslator(&m_translator);
        QLocale::setDefault(QLocale(language));
        m_engine->retranslate();
    }
    if (m_window) win::setDarkFrame(m_window, object.value(u"dark"_s).toBool());
    const QJsonObject values = object.value(u"values"_s).toObject();
    for (auto it = values.begin(); it != values.end(); ++it) {
        const QVariant next = it.value().toVariant();
        if (value(it.key()) == next) continue;
        insert(it.key(), next);
        const QByteArray signal = it.key().toUtf8() + "Changed";
        if (metaObject()->indexOfSignal((signal + "()").constData()) >= 0)
            QMetaObject::invokeMethod(this, signal.constData(), Qt::DirectConnection);
    }
    const auto box = object.value(u"box"_s).toArray();
    if (m_window && box.size() == 4 && !m_placement->moving() && !m_placement->windowSizing())
        // The display's QML owns its size (preview, font scale, user resize).
        // The cloaked source scene can still have the previous preview width.
        m_placement->placeDisplay(QRect(QPoint(box[0].toInt(), box[1].toInt()), m_window->size()),
            object.value(u"room"_s).toInt(), object.value(u"above"_s).toBool());
    m_applying = false;
}

void DisplayReplica::deliverKey(const QJsonObject& object)
{
    if (!m_window || !m_window->isVisible()) return;
    QQuickItem* item = m_window->contentItem();
    while (item->isFocusScope() && item->scopedFocusItem() && item->scopedFocusItem()->isEnabled()) item = item->scopedFocusItem();
    QKeyEvent event(object.value(u"press"_s).toBool() ? QEvent::KeyPress : QEvent::KeyRelease,
        object.value(u"key"_s).toInt(), Qt::KeyboardModifiers(object.value(u"modifiers"_s).toInt()),
        quint32(object.value(u"scan"_s).toInteger()), quint32(object.value(u"vk"_s).toInteger()), 0,
        object.value(u"text"_s).toString(), object.value(u"repeat"_s).toBool());
    if (event.type() == QEvent::KeyPress && event.matches(QKeySequence::Paste) && textfield::pasteOneLine(item)) return;
    for (; item; item = item->parentItem()) {
        event.setAccepted(true);
        QCoreApplication::sendEvent(item, &event);
        if (event.isAccepted()) break;
    }
}

void DisplayReplica::dispatch(const QJsonObject& object)
{
    const QString type = object.value(u"type"_s).toString();
    if (type == u"font" && !m_fontReady) {
        uifont::apply(object.value(u"family"_s).toString());
        m_fontReady = true;
    } else if (type == u"reply") {
        if (!object.value(u"ok"_s).toBool()) { failed(); return; }
        m_replies.insert(object.value(u"id"_s).toInteger(), object.value(u"value"_s).toVariant());
    } else if (type == u"state") {
        applyState(object);
    } else if (type == u"modelBegin") {
        if (!m_rows.begin(object.value(u"count"_s).toInt(-1), object.value(u"selected"_s).toInt())) failed();
    } else if (type == u"modelRows") {
        if (!m_rows.append(object.value(u"from"_s).toInt(-1), object.value(u"rows"_s).toArray())) failed();
    } else if (type == u"modelEnd") {
        if (!m_rows.commit()) failed();
    } else if (type == u"image") {
        const auto response = m_images.take(object.value(u"id"_s).toInteger());
        if (response) {
            QByteArray png = QByteArray::fromBase64(object.value(u"png"_s).toString().toLatin1());
            QBuffer buffer(&png);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer, "PNG");
            const QSize size = reader.size();
            const QImage image = size.width() <= 256 && size.height() <= 256 && size.width() > 0 && size.height() > 0
                ? reader.read() : QImage();
            QMetaObject::invokeMethod(response, [response, image] { if (response) response->finish(image); }, Qt::QueuedConnection);
        }
    } else if (type == u"show" && m_window) {
        m_applying = true;
        m_frame->setNoActivate(true);
        m_window->setProperty("_q_showWithoutActivating", true);
        m_window->show();
        m_window->setProperty("_q_showWithoutActivating", QVariant());
        ::ShowWindow(reinterpret_cast<HWND>(m_window->winId()), SW_SHOWNOACTIVATE);
        ::SetWindowPos(reinterpret_cast<HWND>(m_window->winId()), HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
        m_applying = false;
    } else if (type == u"hide" && m_window) {
        m_applying = true;
        m_window->hide();
        m_applying = false;
    } else if (type == u"activate" && m_window) {
        m_frame->setNoActivate(false);
        win::bringToFront(m_window, win::ShellFlyout::Preserve);
        m_window->requestActivate();
        ::AllowSetForegroundWindow(m_parentPid);
    } else if (type == u"passive" && m_window) {
        m_frame->setNoActivate(true);
        ::AllowSetForegroundWindow(m_parentPid);
    } else if (type == u"key") {
        deliverKey(object);
    } else if (type == u"releasePointer" && m_window) {
        POINT point {};
        ::GetCursorPos(&point);
        const HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
        ::ScreenToClient(hwnd, &point);
        ::PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(short(point.x), short(point.y)));
    } else if (type == u"event") {
        const QString name = object.value(u"name"_s).toString();
        m_applying = true;
        const QJsonArray args = object.value(u"args"_s).toArray();
        if (name == u"groupNameRequested" && args.size() == 2) emit groupNameRequested(args[0].toInt(), args[1].toInteger());
        else if (name == u"stepped" && args.size() == 1) emit stepped(args[0].toInt());
        else if (QStringList {u"shown"_s, u"placed"_s, u"listChanged"_s, u"browseStepRequested"_s,
                     u"contextMenuKeyPressed"_s, u"pinyinHintRequested"_s}.contains(name))
            QMetaObject::invokeMethod(this, name.toUtf8().constData(), Qt::DirectConnection);
        m_applying = false;
    }
}

namespace {
class DisplayImages final : public QQuickAsyncImageProvider {
public:
    explicit DisplayImages(DisplayReplica* bridge) : m_bridge(bridge) {}
    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& size) override
    {
        auto* response = new DisplayImage;
        const QPointer<DisplayReplica> bridge = m_bridge;
        QMetaObject::invokeMethod(m_bridge, [bridge, id, size, response = QPointer<DisplayImage>(response)] {
            if (bridge && response) bridge->requestImage(id, size, response);
        }, Qt::QueuedConnection);
        return response;
    }
private:
    DisplayReplica* m_bridge;
};

QString argument(const QStringList& args, QStringView name)
{
    const int index = args.indexOf(name);
    return index >= 0 && index + 1 < args.size() ? args[index + 1] : QString();
}
} // namespace

bool clipboardDisplayRequested(const QStringList& args) { return args.contains(u"--clipboard-display"_s); }

int runClipboardDisplay(const QStringList& args)
{
    HANDLE token = nullptr;
    DWORD uiAccess = 0, bytes = 0;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        ::GetTokenInformation(token, TokenUIAccess, &uiAccess, sizeof uiAccess, &bytes);
        ::CloseHandle(token);
    }
    const bool probe = args.contains(u"--display-probe"_s);
    if (!uiAccess && !probe) return ERROR_ACCESS_DENIED;
    const DWORD parent = argument(args, u"--display-client").toUInt();
    const HWND source = reinterpret_cast<HWND>(quintptr(argument(args, u"--display-window").toULongLong()));
    DWORD sourceOwner = 0;
    const QString pipe = argument(args, u"--display-pipe");
    if (!parent || !::GetWindowThreadProcessId(source, &sourceOwner) || sourceOwner != parent
        || !pipe.startsWith(u"WinShun.Clipboard.%1."_s.arg(parent))) return ERROR_INVALID_PARAMETER;
    QGuiApplication::setQuitOnLastWindowClosed(false);
    QGuiApplication::clipboard()->blockSignals(true);
    SystemTheme::setBackdropAvailable(win::backdropSupported());
    QQmlApplicationEngine engine;
    DisplayReplica replica(&engine);
    if (!replica.connectTo(pipe, parent)) return ERROR_ACCESS_DENIED;
    engine.addImageProvider(u"fileicon"_s, new DisplayImages(&replica));
    engine.addImageProvider(u"clipthumb"_s, new ClipThumbnailProvider); // the pictures' own files, as before
    QTemporaryDir profile;
    Placement placement(profile.filePath(u"state.ini"_s), u"Clipboard"_s);
    WindowFrame frame(false);
    QQmlComponent component(&engine, u"WinShun"_s, u"ClipboardWindow"_s);
    QObject* root = component.createWithInitialProperties({{u"clipboard"_s, QVariant::fromValue<QObject*>(&replica)},
        {u"placement"_s, QVariant::fromValue(&placement)}, {u"frame"_s, QVariant::fromValue(&frame)}});
    std::unique_ptr<QQuickWindow> window(qobject_cast<QQuickWindow*>(root));
    if (!window) { delete root; qWarning().noquote() << component.errorString(); return 4; }
    window->create();
    placement.setWindow(window.get());
    placement.enableWindowSizing();
    frame.setWindow(window.get());
    frame.setNoActivate(true);
    frame.setResizable(true);
    win::styleFramelessWindow(window.get(), SystemTheme::backdropAvailable());
    win::keepDarkFrameOnShow(window.get());
    replica.setWindow(window.get(), &placement, &frame);
    if (probe) {
        win::setCloaked(window.get(), true); // probes exercise input/state without displaying on the user's desktop
        const auto* field = window->findChild<QQuickItem*>(u"clipboardSearchField"_s);
        const QString family = field ? field->property("font").value<QFont>().family() : QString();
        std::fprintf(stderr, "clipboard-display-font: %s\n", qUtf8Printable(family));
        std::fflush(stderr);
    }
    ContextMenuGuard guard;
    QGuiApplication::instance()->installEventFilter(&guard);
    using GetBand = BOOL(WINAPI*)(HWND, DWORD*);
    const auto getBand = reinterpret_cast<GetBand>(::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetWindowBand"));
    DWORD band = 0;
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!probe && (!getBand || !getBand(hwnd, &band) || band != 2)) return ERROR_NOT_SUPPORTED;
    if (probe && getBand) getBand(hwnd, &band);
    replica.announce(uiAccess != 0, band);
    return QGuiApplication::exec();
}

} // namespace ws
