#include "KeyPresser.h"

#include "DiagnosticLog.h"
#include "OwnInput.h"
#include "platform/Integrity.h"
#include "platform/Paster.h"

#include <QJsonObject>

#include <iterator>
#include <vector>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace ws {

namespace {

bool held(int vk)
{
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
}

// The keys as SendInput takes them, marked as Win顺's own (OwnInput.h):
// its hooks pass over them. A scan code too, for programs that read those;
// 0xE0 before it, an extended key (0xE1 is Pause's, which is none).
std::vector<INPUT> inputs(const std::vector<pastekeys::Key>& keys)
{
    std::vector<INPUT> out;
    out.reserve(keys.size());
    for (const pastekeys::Key& key : keys) {
        const UINT scan = ::MapVirtualKeyW(key.vk, MAPVK_VK_TO_VSC_EX);
        const bool extended = key.extended || (scan & 0xFF00) == 0xE000;
        INPUT input {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = key.vk;
        input.ki.wScan = static_cast<WORD>(scan & 0xFF);
        input.ki.dwFlags = (key.up ? KEYEVENTF_KEYUP : 0) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
        input.ki.dwExtraInfo = kOwnInput;
        out.push_back(input);
    }
    return out;
}

QString classOf(HWND window)
{
    wchar_t name[64] {};
    if (window)
        ::GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return QString::fromWCharArray(name);
}

} // namespace

KeyPresser::KeyPresser(QObject* parent, Sender send)
    : QObject(parent)
    , m_send(send)
{
    m_timer.setInterval(10ms);
    connect(&m_timer, &QTimer::timeout, this, &KeyPresser::step);
}

void KeyPresser::press(const keychord::Chord& chord, HWND target, const QString& name)
{
    m_timer.stop();
    m_chord = chord;
    m_name = name;
    m_target = paste::usableTarget(target);
    m_clock.start();
    m_frontClock.invalidate();
    if (!keychord::sendable(chord)) {
        fail(tr("Windows 不允许程序按这个组合键，只能在键盘上按"), "not-sendable");
        return;
    }
    // While Win顺's own window is in front, Windows lets it give the front away.
    if (m_target && !paste::isForeground(m_target))
        paste::activate(m_target);
    m_timer.start();
}

void KeyPresser::step()
{
    const HWND front = ::GetForegroundWindow();
    DWORD pid = 0;
    if (front)
        ::GetWindowThreadProcessId(front, &pid);
    const bool inFront = m_target ? paste::isForeground(m_target) : front && pid != ::GetCurrentProcessId();
    if (!inFront) {
        m_frontClock.invalidate();
        if (m_clock.elapsed() >= kFrontWaitMs)
            fail(tr("原来的窗口没有回到前面。请先点一下要用它的窗口，再试一次"), "not-in-front");
        return;
    }
    const HWND root = ::GetAncestor(front, GA_ROOT);
    if (integrity::higherThanOurs(root)) {
        fail(tr("前面的窗口以管理员身份运行，Windows 不允许 Win顺 向它发送按键"), "elevated");
        return;
    }
    // Enter, Space (a tile's) or a modifier still down would join the chord.
    if (paste::modifiersDown() || held(VK_RETURN) || held(VK_SPACE)) {
        m_frontClock.invalidate();
        if (m_clock.elapsed() >= kKeysWaitMs)
            fail(tr("有按键一直按着。请松开键盘上的键，再试一次"), "keys-held");
        return;
    }
    if (!m_frontClock.isValid())
        m_frontClock.start();
    if (m_frontClock.elapsed() < kSettleMs)
        return; // in front a moment, for its focus to settle
    m_timer.stop();
    std::vector<INPUT> keys = inputs(keychord::presses(m_chord));
    const auto count = static_cast<UINT>(keys.size());
    ::SetLastError(ERROR_SUCCESS);
    const UINT sent = m_send(count, keys.data(), sizeof(INPUT));
    const DWORD error = sent == count ? ERROR_SUCCESS : ::GetLastError();
    if (sent > 0 && sent < count) {
        // Cut short: what it pressed let go of, nothing left held down.
        std::vector<INPUT> ups;
        for (auto it = keys.begin() + sent; it != keys.end(); ++it) {
            if (it->ki.dwFlags & KEYEVENTF_KEYUP)
                ups.push_back(*it);
        }
        if (!ups.empty())
            m_send(static_cast<UINT>(ups.size()), ups.data(), sizeof(INPUT));
    }
    diagnostics::record(u"favorite-keys"_s,
        {{u"outcome"_s, sent == count ? u"pressed"_s : u"failed"_s}, {u"keys"_s, keychord::text(m_chord)},
         {u"sent"_s, static_cast<int>(sent)}, {u"requested"_s, static_cast<int>(count)},
         {u"error"_s, static_cast<qint64>(error)}, {u"targeted"_s, m_target != nullptr},
         {u"front_class"_s, classOf(root)}, {u"ms"_s, m_clock.elapsed()}});
    if (sent != count)
        emit failed(tr("没有按下“%1”").arg(m_name), tr("Windows 没有接受这些按键，请再试一次"));
}

void KeyPresser::fail(const QString& why, const char* reason)
{
    m_timer.stop();
    diagnostics::record(u"favorite-keys"_s,
        {{u"outcome"_s, u"failed"_s}, {u"reason"_s, QString::fromLatin1(reason)}, {u"keys"_s, keychord::text(m_chord)},
         {u"targeted"_s, m_target != nullptr}, {u"front_class"_s, classOf(::GetForegroundWindow())},
         {u"ms"_s, m_clock.elapsed()}});
    emit failed(tr("没有按下“%1”").arg(m_name), why);
}

} // namespace ws
