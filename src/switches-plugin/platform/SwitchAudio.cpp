// <unknwn.h> before C++/WinRT: its implements<> takes the classic COM
// interfaces then (IMMNotificationClient, IAudioEndpointVolumeCallback).
// Both before Qt's headers: Qt's "signals" and "slots" macros would rewrite
// what is in them.
#include <unknwn.h>
#include <winrt/base.h>

#include "SwitchAudio.h"

#include <QDebug>

#include <windows.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <utility>

using namespace Qt::StringLiterals;

namespace ws {

namespace {

// Win顺's own writes, as the volume callback sees them: read back already.
constexpr GUID kOwnWrite {0x4363e500, 0x30a7, 0x404f, {0xa1, 0x6d, 0x69, 0xe8, 0xeb, 0x86, 0x24, 0xd7}};
// PKEY_Device_FriendlyName, without initguid.h.
constexpr PROPERTYKEY kFriendlyName {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

QString hex(HRESULT hr)
{
    return u"0x%1"_s.arg(static_cast<quint32>(hr), 8, 16, QChar(u'0'));
}

// The level or the mute changed, by another program or the volume keys. On
// a thread of Core Audio's, not always the same one.
struct VolumeCallback : winrt::implements<VolumeCallback, IAudioEndpointVolumeCallback> {
    explicit VolumeCallback(std::shared_ptr<SwitchRelay> relay)
        : m_relay(std::move(relay))
    {
    }

    HRESULT __stdcall OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) noexcept override
    {
        if (!data || data->guidEventContext != kOwnWrite)
            (*m_relay)(SwitchAudio::kId.toString());
        return S_OK;
    }

private:
    std::shared_ptr<SwitchRelay> m_relay;
};

// Another default output, a device come, gone or renamed. Not a call into
// Core Audio from here: the endpoint is bound again at the next read, on the
// switch thread.
struct DeviceCallback : winrt::implements<DeviceCallback, IMMNotificationClient> {
    DeviceCallback(std::shared_ptr<SwitchRelay> relay, std::shared_ptr<std::atomic<bool>> moved)
        : m_relay(std::move(relay))
        , m_moved(std::move(moved))
    {
    }

    HRESULT __stdcall OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) noexcept override
    {
        if (flow == eRender && role == eConsole)
            moved();
        return S_OK;
    }
    HRESULT __stdcall OnDeviceStateChanged(LPCWSTR, DWORD) noexcept override
    {
        moved(); // the default one unplugged or disabled, or one there again
        return S_OK;
    }
    HRESULT __stdcall OnDeviceAdded(LPCWSTR) noexcept override { return S_OK; }
    HRESULT __stdcall OnDeviceRemoved(LPCWSTR) noexcept override { return S_OK; }
    HRESULT __stdcall OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY key) noexcept override
    {
        if (key.fmtid == kFriendlyName.fmtid && key.pid == kFriendlyName.pid)
            moved(); // the second line
        return S_OK;
    }

private:
    void moved()
    {
        m_moved->store(true);
        (*m_relay)(SwitchAudio::kId.toString());
    }

    std::shared_ptr<SwitchRelay> m_relay;
    std::shared_ptr<std::atomic<bool>> m_moved;
};

QString friendlyName(IMMDevice* device)
{
    winrt::com_ptr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.put())))
        return {};
    PROPVARIANT value;
    ::PropVariantInit(&value);
    QString name;
    if (SUCCEEDED(store->GetValue(kFriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
        name = QString::fromWCharArray(value.pwszVal);
    ::PropVariantClear(&value);
    return name;
}

} // namespace

// Everything made on the switch thread, let go there (close).
struct SwitchAudio::Endpoint {
    winrt::com_ptr<IMMDeviceEnumerator> devices;
    winrt::com_ptr<DeviceCallback> deviceCallback; // registered with `devices`
    winrt::com_ptr<VolumeCallback> volumeCallback; // registered with `volume` while there is one
    winrt::com_ptr<IAudioEndpointVolume> volume; // the default output's; null: none, or not bound yet
    bool none = false; // there is no output device
    QString name;

    // The default output now, its changes watched.
    void bind()
    {
        unbind();
        winrt::com_ptr<IMMDevice> device;
        const HRESULT found = devices->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
        if (found == E_NOTFOUND) {
            none = true;
            return;
        }
        if (FAILED(found)) {
            qWarning().noquote() << "SwitchAudio: no default output:" << hex(found);
            return;
        }
        const HRESULT activated = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, volume.put_void());
        if (FAILED(activated)) {
            qWarning().noquote() << "SwitchAudio: the output's volume not there:" << hex(activated);
            volume = nullptr;
            return;
        }
        if (const HRESULT watched = volume->RegisterControlChangeNotify(volumeCallback.get()); FAILED(watched)) {
            qWarning().noquote() << "SwitchAudio: the volume not watched:" << hex(watched);
            volume = nullptr;
            return;
        }
        name = friendlyName(device.get());
    }

    void unbind()
    {
        if (volume)
            volume->UnregisterControlChangeNotify(volumeCallback.get());
        volume = nullptr;
        none = false;
        name.clear();
    }

    // The level and the mute now; false: not read.
    bool get(int& level, bool& muted)
    {
        float scalar = 0;
        BOOL mute = FALSE;
        HRESULT hr = volume->GetMasterVolumeLevelScalar(&scalar);
        if (SUCCEEDED(hr))
            hr = volume->GetMute(&mute);
        if (FAILED(hr)) {
            qWarning().noquote() << "SwitchAudio: not read:" << hex(hr); // AUDCLNT_E_DEVICE_INVALIDATED: gone
            return false;
        }
        level = SwitchAudio::levelOf(scalar);
        muted = mute != FALSE;
        return true;
    }
};

SwitchAudio::SwitchAudio() = default;

SwitchAudio::~SwitchAudio() = default; // close() has let go of the endpoint, on its thread

QStringList SwitchAudio::ids() const
{
    return {kId.toString()};
}

void SwitchAudio::watch(Notify changed)
{
    m_relay->watch(std::move(changed));
}

SwitchAudio::Endpoint* SwitchAudio::endpoint()
{
    if (m_endpoint)
        return m_endpoint.get();
    auto e = std::make_unique<Endpoint>();
    const HRESULT made = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(e->devices.put()));
    if (FAILED(made)) {
        qWarning().noquote() << "SwitchAudio: no device enumerator:" << hex(made);
        return nullptr; // tried again at the next read
    }
    e->deviceCallback = winrt::make_self<DeviceCallback>(m_relay, m_moved);
    e->volumeCallback = winrt::make_self<VolumeCallback>(m_relay);
    if (const HRESULT watched = e->devices->RegisterEndpointNotificationCallback(e->deviceCallback.get()); FAILED(watched)) {
        qWarning().noquote() << "SwitchAudio: the outputs not watched:" << hex(watched);
        e->deviceCallback = nullptr;
    }
    m_endpoint = std::move(e);
    return m_endpoint.get();
}

SwitchState SwitchAudio::read(const QString&)
{
    Endpoint* e = endpoint();
    if (!e)
        return {};
    if (m_moved->exchange(false) || !e->volume)
        e->bind(); // without one: every read looks again (~2 ms), in case a callback was missed
    for (bool again = true;; again = false) {
        if (e->none)
            return noDevice();
        if (!e->volume)
            return {}; // not known: again at the next read
        int level = 0;
        bool muted = false;
        if (e->get(level, muted))
            return stateOf(level, muted, e->name);
        if (!again) {
            e->unbind();
            return {};
        }
        e->bind(); // gone just now (AUDCLNT_E_DEVICE_INVALIDATED): the default one now, once
    }
}

SwitchWrite SwitchAudio::write(const QString& id, const SwitchIntent& intent)
{
    SwitchWrite result;
    result.after = read(id);
    if (!result.after.ready())
        return result; // no output device: its note says so
    const bool muted = result.after.on == SwitchState::Value::Off;
    const std::optional<Target> target = targetOf(result.after.level, muted, intent);
    if (!target) {
        qWarning() << "SwitchAudio: not for the volume:" << static_cast<int>(intent.kind);
        return result;
    }
    if (target->level == result.after.level && target->muted == muted) {
        result.ok = true;
        return result;
    }

    IAudioEndpointVolume* volume = m_endpoint->volume.get(); // bound: read just now
    HRESULT failed = S_OK;
    // The level first: unmuted, the sound comes at the new level only.
    if (target->level != result.after.level) {
        const HRESULT hr = volume->SetMasterVolumeLevelScalar(static_cast<float>(target->level) / 100.0f, &kOwnWrite);
        if (SUCCEEDED(hr))
            result.wrote = true;
        else
            failed = hr;
    }
    if (target->muted != muted) {
        const HRESULT hr = volume->SetMute(target->muted ? TRUE : FALSE, &kOwnWrite);
        if (hr == S_OK)
            result.wrote = true; // S_FALSE: it already was
        else if (FAILED(hr))
            failed = hr;
    }
    if (FAILED(failed)) {
        qWarning().noquote() << "SwitchAudio: not set:" << hex(failed);
        if (failed == AUDCLNT_E_DEVICE_INVALIDATED)
            m_moved->store(true); // the read back binds the default one now
    }

    result.after = read(id);
    const bool mutedNow = result.after.on == SwitchState::Value::Off;
    result.ok = result.after.ready() && result.after.level == target->level && mutedNow == target->muted;
    if (!result.ok && result.after.ready()) {
        const QString why = FAILED(failed) ? QCoreApplication::translate("ws::SwitchAudio", "声音设备没有回应")
                                           : QCoreApplication::translate("ws::SwitchAudio", "被系统改回去了");
        const QString text = mutedNow != target->muted
            ? (target->muted ? QCoreApplication::translate("ws::SwitchAudio", "没能静音：%1")
                             : QCoreApplication::translate("ws::SwitchAudio", "没能取消静音：%1"))
            : QCoreApplication::translate("ws::SwitchAudio", "没能调音量：%1");
        result.error = text.arg(why);
    }
    return result;
}

void SwitchAudio::close()
{
    m_relay->close(); // a call under way has told Switches, or tells nobody
    if (!m_endpoint)
        return;
    m_endpoint->unbind();
    if (m_endpoint->deviceCallback)
        m_endpoint->devices->UnregisterEndpointNotificationCallback(m_endpoint->deviceCallback.get());
    m_endpoint.reset();
}

} // namespace ws
