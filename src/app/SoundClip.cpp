#include "SoundClip.h"

#include <QFile>
#include <QScopeGuard>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace ws {

namespace {

constexpr int kLevelsPerSecond = 100;

struct ComApartment {
    ComApartment()
        : hr(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))
    {
    }
    ~ComApartment()
    {
        if (SUCCEEDED(hr))
            ::CoUninitialize();
    }
    HRESULT hr;
};

// Loudness (RMS of the channels mixed) of every `step` frames, scaled so
// that the loudest is 1. Near silence is 0.
std::vector<float> loudness(const QByteArray& pcm, const WAVEFORMATEX& format, DWORD step)
{
    const auto* samples = reinterpret_cast<const qint16*>(pcm.constData());
    const std::size_t channels = format.nChannels;
    const std::size_t frames = static_cast<std::size_t>(pcm.size()) / format.nBlockAlign;
    std::vector<float> levels;
    levels.reserve(frames / step + 1);
    for (std::size_t first = 0; first < frames; first += step) {
        const std::size_t end = std::min<std::size_t>(frames, first + step);
        double sum = 0;
        for (std::size_t f = first; f < end; ++f) {
            double mixed = 0;
            for (std::size_t c = 0; c < channels; ++c)
                mixed += samples[f * channels + c];
            mixed /= static_cast<double>(channels) * 32768.0;
            sum += mixed * mixed;
        }
        levels.push_back(static_cast<float>(std::sqrt(sum / static_cast<double>(end - first))));
    }
    const float loudest = levels.empty() ? 0.0f : *std::max_element(levels.begin(), levels.end());
    for (float& level : levels)
        level = loudest > 0 && level / loudest >= 0.05f ? level / loudest : 0.0f;
    return levels;
}

// Cuts the silence before and after the sound (keeping 10 ms in front of it),
// so that it is heard the moment it is played and ends when it falls silent.
void trimSilence(QByteArray& pcm, const WAVEFORMATEX& format, DWORD step)
{
    const std::vector<float> levels = loudness(pcm, format, step);
    const auto first = std::find_if(levels.begin(), levels.end(), [](float l) { return l > 0; });
    if (first == levels.end())
        return;
    const auto last = std::find_if(levels.rbegin(), levels.rend(), [](float l) { return l > 0; });
    const qsizetype bytesPerStep = static_cast<qsizetype>(step) * format.nBlockAlign;
    const qsizetype begin = std::max<qsizetype>(0, (first - levels.begin() - 1) * bytesPerStep);
    const qsizetype end = std::min<qsizetype>(pcm.size(), (levels.rend() - last) * bytesPerStep);
    pcm = pcm.mid(begin, end - begin);
}

// Decodes a compressed sound (MP3 and whatever else Media Foundation reads)
// to 16-bit PCM. `type` ("audio/mpeg") tells Media Foundation what it is:
// in memory, it has no file name to go by. Null if it cannot. Runs on a
// worker thread.
std::shared_ptr<SoundClip::Decoded> decode(const QByteArray& file, const std::wstring& type)
{
    // Linked with /DELAYLOAD: Windows N without the Media Feature Pack has
    // no Media Foundation, and WinShun must still start there.
    if (!::LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)
        || !::LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        return nullptr;
    const ComApartment com;
    if (FAILED(::MFStartup(MF_VERSION, MFSTARTUP_LITE)))
        return nullptr;
    const auto shutdown = qScopeGuard([] { ::MFShutdown(); });

    ComPtr<IStream> stream;
    stream.Attach(::SHCreateMemStream(reinterpret_cast<const BYTE*>(file.constData()), static_cast<UINT>(file.size())));
    ComPtr<IMFByteStream> bytes;
    if (!stream || FAILED(::MFCreateMFByteStreamOnStream(stream.Get(), &bytes)))
        return nullptr;
    if (ComPtr<IMFAttributes> attributes; !type.empty() && SUCCEEDED(bytes.As(&attributes)))
        attributes->SetString(MF_BYTESTREAM_CONTENT_TYPE, type.c_str());
    ComPtr<IMFSourceReader> reader;
    if (FAILED(::MFCreateSourceReaderFromByteStream(bytes.Get(), nullptr, &reader)))
        return nullptr;

    const auto audio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    ComPtr<IMFMediaType> wanted;
    if (FAILED(::MFCreateMediaType(&wanted)))
        return nullptr;
    wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    wanted->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    wanted->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(audio, TRUE);
    ComPtr<IMFMediaType> actual;
    if (FAILED(reader->SetCurrentMediaType(audio, nullptr, wanted.Get()))
        || FAILED(reader->GetCurrentMediaType(audio, &actual)))
        return nullptr;

    auto clip = std::make_shared<SoundClip::Decoded>();
    WAVEFORMATEX* format = nullptr;
    UINT32 formatSize = 0;
    if (FAILED(::MFCreateWaveFormatExFromMFMediaType(actual.Get(), &format, &formatSize)))
        return nullptr;
    clip->format = *format; // mono or stereo PCM: no extensible part needed
    ::CoTaskMemFree(format);
    clip->format.wFormatTag = WAVE_FORMAT_PCM;
    clip->format.cbSize = 0;
    if (clip->format.wBitsPerSample != 16 || clip->format.nChannels == 0 || clip->format.nChannels > 2)
        return nullptr;

    for (;;) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(audio, 0, nullptr, &flags, nullptr, &sample)))
            return nullptr;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
        if (!sample)
            continue;
        ComPtr<IMFMediaBuffer> buffer;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || FAILED(buffer->Lock(&data, nullptr, &length)))
            return nullptr;
        clip->pcm.append(reinterpret_cast<const char*>(data), static_cast<qsizetype>(length));
        buffer->Unlock();
    }
    clip->pcm.truncate(clip->pcm.size() - clip->pcm.size() % clip->format.nBlockAlign);
    if (clip->pcm.isEmpty())
        return nullptr;
    clip->step = std::max<DWORD>(1, clip->format.nSamplesPerSec / kLevelsPerSecond);
    trimSilence(clip->pcm, clip->format, clip->step);
    clip->levels = loudness(clip->pcm, clip->format, clip->step);
    return clip;
}

} // namespace

SoundClip::SoundClip(QObject* parent)
    : QObject(parent)
{
    m_watch.setInterval(100);
    connect(&m_watch, &QTimer::timeout, this, [this] {
        if (m_header.dwFlags & WHDR_DONE) // played to the end
            stop();
    });
}

SoundClip::~SoundClip()
{
    if (m_decoder.joinable())
        m_decoder.join();
    stop();
}

void SoundClip::setSource(const QString& source)
{
    if (source == m_source)
        return;
    stop();
    m_source = source;
    m_clip.reset();
    if (m_failed) {
        m_failed = false;
        emit availableChanged();
    }
    emit sourceChanged();
}

void SoundClip::prepare()
{
    if (m_clip || m_failed || m_decoding || m_source.isEmpty())
        return;
    QFile file(m_source);
    if (!file.open(QIODevice::ReadOnly)) {
        m_failed = true;
        emit availableChanged();
        return;
    }
    if (m_decoder.joinable())
        m_decoder.join(); // an earlier one, long done
    const QString suffix = m_source.section(u'.', -1).toLower();
    std::wstring type = suffix == u"mp3" ? L"audio/mpeg" : suffix == u"wav" ? L"audio/wav" : L"";
    m_decoding = true;
    m_decoder = std::jthread([this, data = file.readAll(), type = std::move(type)] {
        std::shared_ptr<const Decoded> clip = decode(data, type);
        QMetaObject::invokeMethod(this, [this, clip] { decoded(clip); }, Qt::QueuedConnection);
    });
}

void SoundClip::decoded(std::shared_ptr<const Decoded> clip)
{
    m_decoding = false;
    if (!clip) {
        m_playWhenReady = false;
        m_failed = true;
        emit availableChanged();
        return;
    }
    m_clip = std::move(clip);
    if (std::exchange(m_playWhenReady, false))
        start();
}

void SoundClip::play()
{
    if (m_failed)
        return;
    if (!m_clip) {
        m_playWhenReady = true;
        prepare();
        return;
    }
    if (m_device && !(m_header.dwFlags & WHDR_DONE))
        return; // still playing
    stop(); // over, though m_watch has not noticed yet
    start();
}

void SoundClip::start()
{
    HWAVEOUT device = nullptr;
    if (::waveOutOpen(&device, WAVE_MAPPER, &m_clip->format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        return; // no output (none plugged in, say): maybe next time
    m_header = {};
    m_header.lpData = const_cast<char*>(m_clip->pcm.constData()); // only read
    m_header.dwBufferLength = static_cast<DWORD>(m_clip->pcm.size());
    if (::waveOutPrepareHeader(device, &m_header, sizeof m_header) != MMSYSERR_NOERROR) {
        ::waveOutClose(device);
        return;
    }
    m_device = device;
    m_watch.start();
    emit playingChanged();
    if (::waveOutWrite(m_device, &m_header, sizeof m_header) != MMSYSERR_NOERROR)
        stop();
}

void SoundClip::stop()
{
    m_playWhenReady = false;
    if (!m_device)
        return;
    m_watch.stop();
    ::waveOutReset(m_device);
    ::waveOutUnprepareHeader(m_device, &m_header, sizeof m_header);
    ::waveOutClose(m_device);
    m_device = nullptr;
    emit playingChanged();
}

double SoundClip::level() const
{
    if (!m_device || !m_clip || m_clip->levels.empty())
        return 0;
    MMTIME time {};
    time.wType = TIME_SAMPLES;
    if (::waveOutGetPosition(m_device, &time, sizeof time) != MMSYSERR_NOERROR)
        return 0;
    double frame = 0;
    if (time.wType == TIME_SAMPLES)
        frame = time.u.sample;
    else if (time.wType == TIME_BYTES)
        frame = static_cast<double>(time.u.cb) / m_clip->format.nBlockAlign;
    else
        return 0;
    // Between two measurements, a straight line.
    const double at = frame / m_clip->step;
    const auto i = static_cast<std::size_t>(at);
    const std::vector<float>& levels = m_clip->levels;
    if (i + 1 >= levels.size())
        return i < levels.size() ? levels[i] : 0.0;
    const double f = at - static_cast<double>(i);
    return levels[i] * (1 - f) + levels[i + 1] * f;
}

} // namespace ws
