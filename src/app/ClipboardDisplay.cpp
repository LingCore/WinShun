#include "ClipboardDisplay.h"

#include "Clipboard.h"
#include "DiagnosticLog.h"
#include "FileIconProvider.h"
#include "Placement.h"
#include "Settings.h"
#include "SystemTheme.h"
#include "VolumeProtocol.h"
#include "platform/WindowEffects.h"

#include <QBuffer>
#include <QGuiApplication>
#include <QLocale>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QQuickWindow>
#include <QScreen>
#include <QStyleHints>
#include <QUuid>

#include <algorithm>
#include <array>
#include <utility>

using namespace Qt::StringLiterals;

namespace ws {
namespace {

QVariant call(Clipboard* object, const QString& name, const QVariantList& values, bool& ok)
{
    static const QStringList allowed {
        u"heldRow"_s, u"paste"_s, u"quickPaste"_s, u"copy"_s, u"dragOut"_s, u"copyText"_s,
        u"togglePin"_s, u"remove"_s, u"undo"_s, u"moveToGroup"_s, u"canMove"_s, u"moveRow"_s,
        u"addGroup"_s, u"renameGroup"_s, u"toggleSelected"_s, u"selectRange"_s, u"clearSelection"_s,
        u"selectAll"_s, u"cycleCategory"_s, u"cycleSeparator"_s, u"dismiss"_s, u"wantKeys"_s,
        u"turnOn"_s, u"preview"_s, u"menuItems"_s, u"categoryMenuItems"_s, u"trigger"_s,
        u"triggerCategory"_s, u"groupNameRequested"_s};
    ok = false;
    if (!allowed.contains(name) || values.size() > 3)
        return {};
    const QMetaObject* meta = object->metaObject();
    for (int i = Clipboard::staticMetaObject.methodOffset(); i < meta->methodCount(); ++i) {
        const QMetaMethod method = meta->method(i);
        if (method.name() != name.toUtf8() || method.parameterCount() != values.size())
            continue;
        std::array<QVariant, 3> converted;
        std::array<QGenericArgument, 3> arguments;
        bool valid = true;
        for (int j = 0; j < values.size(); ++j) {
            converted[j] = values[j];
            valid &= converted[j].convert(method.parameterMetaType(j));
            arguments[j] = QGenericArgument(method.parameterMetaType(j).name(), converted[j].constData());
        }
        if (!valid)
            return {};
        const QMetaType resultType = method.returnMetaType();
        QVariant result;
        QGenericReturnArgument returned;
        if (resultType.id() != QMetaType::Void) {
            result = QVariant(resultType);
            returned = QGenericReturnArgument(resultType.name(), result.data());
        }
        ok = method.invoke(object, Qt::DirectConnection, returned, arguments[0], arguments[1], arguments[2]);
        return result;
    }
    return {};
}

QJsonValue jsonValue(const QVariant& value)
{
    if (value.metaType().id() == QMetaType::QColor)
        return value.value<QColor>().name(QColor::HexArgb);
    return QJsonValue::fromVariant(value);
}

} // namespace

ClipboardDisplay::ClipboardDisplay(Clipboard* clipboard, QQuickWindow* window, Placement* placement, bool probe, QObject* parent)
    : QObject(parent), m_clipboard(clipboard), m_window(window), m_sourceWindow(reinterpret_cast<HWND>(window->winId())), m_placement(placement)
    , m_stop(::CreateEventW(nullptr, TRUE, FALSE, nullptr))
    , m_probe(probe)
{
    m_icons.setMaxThreadCount(2);
    m_update.setSingleShot(true);
    connect(&m_update, &QTimer::timeout, this, &ClipboardDisplay::synchronize);
    connect(&m_server, &QLocalServer::newConnection, this, &ClipboardDisplay::accept);
    const QMetaMethod schedule = metaObject()->method(metaObject()->indexOfSlot("schedule()"));
    const QMetaObject* meta = clipboard->metaObject();
    for (int i = Clipboard::staticMetaObject.propertyOffset(); i < meta->propertyCount(); ++i) {
        const QMetaProperty property = meta->property(i);
        if (property.hasNotifySignal())
            QObject::connect(clipboard, property.notifySignal(), this, schedule);
    }
    auto* model = clipboard->items();
    connect(model, &QAbstractItemModel::modelReset, this, &ClipboardDisplay::schedule);
    connect(model, &QAbstractItemModel::rowsInserted, this, &ClipboardDisplay::schedule);
    connect(model, &QAbstractItemModel::rowsRemoved, this, &ClipboardDisplay::schedule);
    connect(model, &QAbstractItemModel::rowsMoved, this, &ClipboardDisplay::schedule);
    connect(model, &QAbstractItemModel::dataChanged, this, &ClipboardDisplay::schedule);
    connect(model, &ClipModel::selectionChanged, this, &ClipboardDisplay::schedule);
    connect(window, &QWindow::xChanged, this, &ClipboardDisplay::schedule);
    connect(window, &QWindow::yChanged, this, &ClipboardDisplay::schedule);
    connect(window, &QWindow::widthChanged, this, &ClipboardDisplay::schedule);
    connect(window, &QWindow::heightChanged, this, &ClipboardDisplay::schedule);
    connect(clipboard, &Clipboard::groupNameRequested, this, [this](int row, qint64 group) {
        event(u"groupNameRequested"_s, {row, group});
    });
    connect(clipboard, &Clipboard::stepped, this, [this](int row) { event(u"stepped"_s, {row}); });
    connect(clipboard, &Clipboard::browseStepRequested, this, [this] { event(u"browseStepRequested"_s); });
    connect(clipboard, &Clipboard::listChanged, this, [this] { event(u"listChanged"_s); });
    connect(clipboard, &Clipboard::pinyinHintRequested, this, [this] { event(u"pinyinHintRequested"_s); });
    connect(clipboard, &Clipboard::contextMenuKeyPressed, this, [this] { event(u"contextMenuKeyPressed"_s); });
    connect(clipboard, &Clipboard::placed, this, [this] { event(u"placed"_s); });
}

ClipboardDisplay::~ClipboardDisplay()
{
    m_active = false;
    m_ready = false;
    m_icons.waitForDone(); // icon rendering uses the GUI's font/database lifetime
    if (m_probeChild.state() != QProcess::NotRunning) {
        m_probeChild.terminate();
        if (!m_probeChild.waitForFinished(1000)) { m_probeChild.kill(); m_probeChild.waitForFinished(1000); }
    }
    if (m_stop) ::SetEvent(m_stop);
    if (m_lease.joinable()) m_lease.join();
    if (m_stop) ::CloseHandle(m_stop);
    if (m_socket) m_socket->abort();
}

void ClipboardDisplay::start()
{
    if (m_lease.joinable() || m_server.isListening() || !m_stop)
        return;
    m_pipe = u"WinShun.Clipboard.%1.%2"_s.arg(::GetCurrentProcessId()).arg(
        QUuid::createUuid().toString(QUuid::Id128));
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server.listen(m_pipe))
        return;
    if (m_probe) {
        m_probeChild.setProcessChannelMode(QProcess::MergedChannels);
        m_probeChild.start(QCoreApplication::applicationFilePath(), {u"--clipboard-display"_s, u"--display-client"_s,
            QString::number(::GetCurrentProcessId()), u"--display-window"_s, QString::number(quintptr(m_sourceWindow)),
            u"--display-pipe"_s, m_pipe, u"--display-probe"_s});
        if (m_probeChild.waitForStarted(1000)) m_peerPid = DWORD(m_probeChild.processId());
        accept();
        return;
    }
    const std::wstring pipe = m_pipe.toStdWString();
    m_lease = std::thread([this, pipe] {
        do { lease(pipe); } while (::WaitForSingleObject(m_stop, 5000) == WAIT_TIMEOUT);
    });
}

void ClipboardDisplay::lease(const std::wstring& pipe)
{
    using namespace volproto;
    wchar_t overrideName[256] {};
    const DWORD n = ::GetEnvironmentVariableW(kPipeEnvironment, overrideName, 256);
    const std::wstring path = fullPipePath(n && n < 256 ? overrideName : kPipeName);
    Channel channel(::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    constexpr DWORD timeout = 1000;
    Writer writer;
    Hello hello;
    hello.pid = ::GetCurrentProcessId();
    encode(writer, hello);
    Type type {};
    std::vector<std::byte> payload;
    HelloOk greeted;
    if (!channel.valid() || !channel.write(writer.finish(Type::Hello), timeout)
        || !channel.read(type, payload, timeout, m_stop) || type != Type::HelloOk || !decode(payload, greeted)
        || greeted.major != kMajor || !(greeted.capabilities & kCapabilityClipboardDisplay))
        return;
    writer.reset();
    writer.u64(reinterpret_cast<std::uintptr_t>(m_sourceWindow));
    writer.string(pipe);
    if (!channel.write(writer.finish(Type::OpenClipboardDisplay), timeout)
        || !channel.read(type, payload, timeout, m_stop) || type != Type::ClipboardDisplayReady)
        return;
    Reader reader(payload);
    std::uint32_t pid = 0;
    if (!reader.u32(pid) || !pid || !reader.done())
        return;
    m_peerPid = pid;
    QMetaObject::invokeMethod(this, &ClipboardDisplay::accept, Qt::QueuedConnection);
    while (::WaitForSingleObject(m_stop, 1000) == WAIT_TIMEOUT) {
        if (!channel.write(Type::ClipboardDisplayPing, {}, timeout)
            || !channel.read(type, payload, timeout, m_stop) || type != Type::ClipboardDisplayPing || !payload.empty())
            break;
    }
    m_peerPid = 0;
    QMetaObject::invokeMethod(this, &ClipboardDisplay::disconnectPeer, Qt::QueuedConnection);
}

void ClipboardDisplay::accept()
{
    if (!m_peerPid)
        return; // the child can connect before the service's Ready frame
    while (m_server.hasPendingConnections()) {
        QLocalSocket* socket = m_server.nextPendingConnection();
        ULONG pid = 0;
        if (m_socket || !::GetNamedPipeClientProcessId(reinterpret_cast<HANDLE>(socket->socketDescriptor()), &pid)
            || pid != m_peerPid) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        m_socket = socket;
        socket->setReadBufferSize(2 * clipdisplay::kMaxFrame);
        m_decoder = {};
        connect(socket, &QLocalSocket::readyRead, this, &ClipboardDisplay::receive);
        connect(socket, &QLocalSocket::disconnected, this, &ClipboardDisplay::disconnectPeer);
        receive();
    }
}

void ClipboardDisplay::disconnectPeer()
{
    const bool wasActive = std::exchange(m_active, false);
    m_ready = false;
    m_handle = nullptr;
    m_rows.clear();
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket.clear();
    }
    if (wasActive) emit lost();
}

void ClipboardDisplay::send(const QJsonObject& message)
{
    if (!m_socket)
        return;
    const QByteArray bytes = clipdisplay::frame(message);
    if (bytes.isEmpty() || m_socket->bytesToWrite() > 8 * clipdisplay::kMaxFrame) {
        disconnectPeer();
        return;
    }
    m_socket->write(bytes);
    m_socket->flush();
}

void ClipboardDisplay::receive()
{
    if (!m_socket || !m_decoder.append(m_socket->readAll())) {
        disconnectPeer();
        return;
    }
    QJsonObject message;
    while (m_socket && m_decoder.take(message)) dispatch(message);
    if (m_decoder.failed()) disconnectPeer();
}

void ClipboardDisplay::dispatch(const QJsonObject& message)
{
    const QString type = message.value(u"type"_s).toString();
    if (type == u"ready") {
        const HWND hwnd = reinterpret_cast<HWND>(message.value(u"window"_s).toString().toULongLong());
        DWORD owner = 0;
        ::GetWindowThreadProcessId(hwnd, &owner);
        if (!hwnd || owner != m_peerPid || (!m_probe && (!message.value(u"uiAccess"_s).toBool()
            || message.value(u"band"_s).toInt() != 2))) {
            disconnectPeer();
            return;
        }
        m_handle = hwnd;
        m_ready = true;
        diagnostics::record(u"clipboard-display"_s, {{u"state"_s, u"ready"_s}, {u"band"_s, message.value(u"band"_s)}});
        emit available();
        return;
    }
    if (!m_ready)
        return;
    if (type == u"focusLost") { if (m_active) emit focusLost(); return; }
    if (type == u"geometry") {
        const QJsonArray box = message.value(u"box"_s).toArray();
        if (box.size() == 4) emit geometryEdited({box[0].toInt(), box[1].toInt(), box[2].toInt(), box[3].toInt()});
        return;
    }
    if (type == u"closed") {
        m_clipboard->setPinned(false);
        m_clipboard->dismiss();
        return;
    }
    if (type == u"set") {
        const QByteArray name = message.value(u"name"_s).toString().toUtf8();
        const int index = m_clipboard->metaObject()->indexOfProperty(name.constData());
        if (index >= Clipboard::staticMetaObject.propertyOffset()) {
            const QMetaProperty property = m_clipboard->metaObject()->property(index);
            if (property.isWritable()) property.write(m_clipboard, message.value(u"value"_s).toVariant());
        }
        return;
    }
    if (type == u"highlight") {
        m_clipboard->items()->setHighlightColor(QColor(message.value(u"color"_s).toString()));
        return;
    }
    if (type == u"image") {
        const qint64 id = message.value(u"id"_s).toInteger();
        const QString key = message.value(u"key"_s).toString();
        const QSize size(std::clamp(message.value(u"width"_s).toInt(), 1, 256),
            std::clamp(message.value(u"height"_s).toInt(), 1, 256));
        if (key.size() > 8192)
            return;
        if (m_imageRequests >= 128) {
            send({{u"type"_s, u"image"_s}, {u"id"_s, id}, {u"png"_s, QString()}});
            return;
        }
        ++m_imageRequests;
        const QPointer<ClipboardDisplay> self(this);
        m_icons.start([self, id, key, size] {
            FileIconProvider provider;
            QSize actualSize;
            QImage image = provider.requestImage(key, &actualSize, size);
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            if (!image.isNull()) image.save(&buffer, "PNG");
            if (self) QMetaObject::invokeMethod(self, [self, id, png] {
                if (self) {
                    --self->m_imageRequests;
                    self->send({{u"type"_s, u"image"_s}, {u"id"_s, id}, {u"png"_s, QString::fromLatin1(png.toBase64())}});
                }
            }, Qt::QueuedConnection);
        });
        return;
    }
    if (type == u"call") {
        bool ok = false;
        const QVariant result = call(m_clipboard, message.value(u"name"_s).toString(), message.value(u"args"_s).toArray().toVariantList(), ok);
        if (message.value(u"name"_s).toString() == u"dragOut") send({{u"type"_s, u"releasePointer"_s}});
        if (message.value(u"id"_s).toInteger() != 0)
            send({{u"type"_s, u"reply"_s}, {u"id"_s, message.value(u"id"_s)}, {u"ok"_s, ok}, {u"value"_s, jsonValue(result)}});
    }
}

void ClipboardDisplay::schedule()
{
    if (m_active && !m_update.isActive()) m_update.start(0);
}

void ClipboardDisplay::synchronize()
{
    if (!m_active || !m_ready)
        return;
    QJsonObject properties;
    const QMetaObject* meta = m_clipboard->metaObject();
    for (int i = Clipboard::staticMetaObject.propertyOffset(); i < meta->propertyCount(); ++i) {
        const auto property = meta->property(i);
        const QByteArray name = property.name();
        if (name != "items" && name != "field")
            properties.insert(QString::fromLatin1(name), jsonValue(property.read(m_clipboard)));
    }
    const QRect geometry = m_window->geometry();
    send({{u"type"_s, u"state"_s}, {u"values"_s, properties},
        {u"box"_s, QJsonArray {geometry.x(), geometry.y(), geometry.width(), geometry.height()}},
        {u"room"_s, m_placement->room()}, {u"above"_s, m_placement->above()},
        {u"dark"_s, QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark},
        {u"uiSize"_s, SystemTheme::uiSize()}, {u"language"_s, QLocale().name()},
        {u"rounded"_s, win::roundedCorners()}, {u"backdrop"_s, SystemTheme::backdrop()}});
    ClipModel* model = m_clipboard->items();
    const auto roles = model->roleNames();
    std::vector<QJsonObject> rows;
    rows.reserve(model->count());
    for (int i = 0; i < model->count(); ++i) {
        QJsonObject row;
        for (auto role = roles.cbegin(); role != roles.cend(); ++role)
            row.insert(QString::fromLatin1(role.value()), jsonValue(model->data(model->index(i), role.key())));
        rows.push_back(std::move(row));
    }
    if (rows != m_rows) {
        send({{u"type"_s, u"modelBegin"_s}, {u"count"_s, model->count()}, {u"selected"_s, model->selectedCount()}});
        for (int from = 0; from < rows.size(); from += clipdisplay::kRowsPerFrame) {
            QJsonArray chunk;
            for (int i = from; i < std::min<int>(int(rows.size()), from + clipdisplay::kRowsPerFrame); ++i) chunk.append(rows[i]);
            send({{u"type"_s, u"modelRows"_s}, {u"from"_s, from}, {u"rows"_s, chunk}});
        }
        send({{u"type"_s, u"modelEnd"_s}});
        m_rows = std::move(rows);
    }
}

bool ClipboardDisplay::show()
{
    if (!m_ready)
        return false;
    m_active = true;
    m_rows.clear();
    synchronize();
    event(u"shown"_s);
    event(u"placed"_s); // placement happened before this peer became active
    send({{u"type"_s, u"show"_s}}); // reveal only after restoring the saved preview
    return m_ready && m_active;
}

void ClipboardDisplay::hide()
{
    if (!m_active) return;
    m_active = false; // an intentional close must not reveal a fallback on pipe loss
    send({{u"type"_s, u"hide"_s}});
    m_rows.clear();
}

void ClipboardDisplay::activate()
{
    if (m_active) send({{u"type"_s, u"activate"_s}});
}

void ClipboardDisplay::passive()
{
    if (m_active) send({{u"type"_s, u"passive"_s}});
}

void ClipboardDisplay::key(const KeyRouter::Key& key)
{
    if (m_active) send({{u"type"_s, u"key"_s}, {u"key"_s, key.key}, {u"modifiers"_s, int(key.modifiers)},
        {u"text"_s, key.text}, {u"vk"_s, qint64(key.vk)}, {u"scan"_s, qint64(key.scanCode)},
        {u"press"_s, key.press}, {u"repeat"_s, key.autoRepeat}});
}

void ClipboardDisplay::event(const QString& name, const QJsonArray& arguments)
{
    if (!m_active) return;
    synchronize(); // event handlers see the state that caused the signal
    send({{u"type"_s, u"event"_s}, {u"name"_s, name}, {u"args"_s, arguments}});
}

} // namespace ws
