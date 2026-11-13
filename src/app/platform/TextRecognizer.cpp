// C++/WinRT before Qt's headers: Qt's "signals" and "slots" macros would
// rewrite what is in it.
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.UserProfile.h>

#include "TextRecognizer.h"

#include "PaddleOcr.h"
#include "QrCode.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QStringList>

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string_view>

using namespace Qt::StringLiterals;
using winrt::Windows::Globalization::Language;
using winrt::Windows::Graphics::Imaging::BitmapAlphaMode;
using winrt::Windows::Graphics::Imaging::BitmapPixelFormat;
using winrt::Windows::Graphics::Imaging::SoftwareBitmap;
using winrt::Windows::Media::Ocr::OcrEngine;
using winrt::Windows::Media::Ocr::OcrLine;

namespace ws {

namespace {

QString toQString(const winrt::hstring& s)
{
    return QString::fromWCharArray(s.c_str(), static_cast<qsizetype>(s.size()));
}

// Written without spaces between words: Chinese, Japanese, their punctuation
// and full-width forms (Korean has spaces).
bool spaceless(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= 0x2E80 && u <= 0x9FFF) || (u >= 0xF900 && u <= 0xFAFF) || (u >= 0xFF00 && u <= 0xFFEF)
        || c.isSurrogate(); // the rarer ideographs, beyond the first plane
}

// A line's words, a space only where the writing has one.
QString lineText(const OcrLine& line)
{
    QString text;
    for (const auto& word : line.Words()) {
        const QString w = toQString(word.Text());
        if (w.isEmpty())
            continue;
        if (!text.isEmpty() && !(spaceless(text.back()) && spaceless(w.front())))
            text += u' ';
        text += w;
    }
    return text;
}

// The first Chinese, Japanese or Korean language in the user's list with a
// recognizer; else what Windows picks for their languages; else any.
OcrEngine pickEngine()
{
    const auto available = OcrEngine::AvailableRecognizerLanguages();
    for (const winrt::hstring& tag : winrt::Windows::System::UserProfile::GlobalizationPreferences::Languages()) {
        const std::wstring_view t = tag;
        if (!t.starts_with(L"zh") && !t.starts_with(L"ja") && !t.starts_with(L"ko"))
            continue;
        const Language wanted(tag);
        for (const Language& language : available) {
            // "zh-CN" in the list is "zh-Hans-CN" here: by language and script.
            const std::wstring_view have = language.LanguageTag();
            if (have.substr(0, 2) != t.substr(0, 2) || (!wanted.Script().empty() && language.Script() != wanted.Script()))
                continue;
            if (OcrEngine engine = OcrEngine::TryCreateFromLanguage(language))
                return engine;
        }
    }
    if (OcrEngine engine = OcrEngine::TryCreateFromUserProfileLanguages())
        return engine;
    return available.Size() > 0 ? OcrEngine::TryCreateFromLanguage(available.GetAt(0)) : OcrEngine {nullptr};
}

// Japanese or Korean before any Chinese in the user's languages: read by
// Windows' recognizer, PP-OCRv6 tiny reads neither.
bool japaneseOrKoreanFirst()
{
    try {
        for (const winrt::hstring& tag : winrt::Windows::System::UserProfile::GlobalizationPreferences::Languages()) {
            const std::wstring_view t = tag;
            if (t.starts_with(L"zh"))
                return false;
            if (t.starts_with(L"ja") || t.starts_with(L"ko"))
                return true;
        }
    } catch (const winrt::hresult_error&) {
    }
    return false;
}

// The picture as the recognizer takes it, no larger than it can read.
SoftwareBitmap loadBitmap(const QString& file, int maxSide)
{
    QImage image(file);
    if (image.isNull())
        return nullptr;
    if (image.width() > maxSide || image.height() > maxSide)
        image = image.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    // See-through parts white: black text there would be read as black on black.
    QImage flat(image.size(), QImage::Format_ARGB32_Premultiplied); // BGRA in memory
    flat.fill(Qt::white);
    {
        QPainter painter(&flat);
        painter.drawImage(0, 0, image);
    }
    const auto bytes = static_cast<uint32_t>(flat.sizeInBytes());
    winrt::Windows::Storage::Streams::Buffer buffer(bytes);
    std::memcpy(buffer.data(), flat.constBits(), bytes);
    buffer.Length(bytes);
    SoftwareBitmap bitmap(BitmapPixelFormat::Bgra8, flat.width(), flat.height(), BitmapAlphaMode::Premultiplied);
    bitmap.CopyFromBuffer(buffer);
    return bitmap;
}

} // namespace

TextRecognizer::TextRecognizer(QObject* parent)
    : QObject(parent)
    , m_paddle(PaddleOcr::installed(QCoreApplication::applicationDirPath()) && !japaneseOrKoreanFirst())
{
}

TextRecognizer::~TextRecognizer()
{
    {
        const std::lock_guard lock(m_mutex);
        m_jobs.clear();
    }
    // Here, not as a member goes: `done` may still be emitted, by a whole object.
    m_thread.request_stop();
    if (m_thread.joinable())
        m_thread.join();
}

void TextRecognizer::read(qint64 id, const QString& imageFile, bool next)
{
    const std::lock_guard lock(m_mutex);
    if (!m_running && m_thread.joinable())
        m_thread.join(); // its last batch has ended; only model cleanup can remain
    if (next)
        m_jobs.push_front({id, imageFile});
    else
        m_jobs.push_back({id, imageFile});
    if (!m_running) {
        m_running = true;
        m_thread = std::jthread([this](std::stop_token stop) { work(stop); });
    }
}

void TextRecognizer::cancel()
{
    const std::lock_guard lock(m_mutex);
    m_jobs.clear();
}

bool TextRecognizer::running() const
{
    const std::lock_guard lock(m_mutex);
    return m_running;
}

QString TextRecognizer::engine() const
{
    return m_paddle ? u"ppocr-v6-tiny"_s : u"windows"_s;
}

bool TextRecognizer::refreshEngine()
{
    const std::lock_guard lock(m_mutex);
    if (m_running)
        return false;
    m_paddle = PaddleOcr::installed(QCoreApplication::applicationDirPath()) && !japaneseOrKoreanFirst();
    return true;
}

void TextRecognizer::work(const std::stop_token& stop)
{
    // A screenshot takes a few tenths of a second by PP-OCRv6 (1902 × 1316 px:
    // ~0.7 s), a tenth by Windows' recognizer (1560 × 1197 px: 107 ms).
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    {
        const QString folder = QCoreApplication::applicationDirPath();
        // Its own threads besides this one: up to 4, half the processors.
        const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 2, 1, 4);
        std::unique_ptr<PaddleOcr> paddle;
        OcrEngine engine {nullptr};
        bool none = false; // no recognizer, found in this batch
        for (;;) {
            Job job;
            {
                std::unique_lock lock(m_mutex);
                if (m_jobs.empty() || stop.stop_requested()) {
                    m_running = false;
                    emit idle();
                    break;
                }
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }
            if (!QFileInfo::exists(job.file)) {
                emit done(job.id, {}, false);
                continue; // never load an OCR model for a missing history image
            }
            QString words;
            bool ok = true;
            if (m_paddle && !paddle) {
                QString error;
                paddle = PaddleOcr::load(folder, threads, &error);
                if (!paddle) {
                    qWarning().noquote() << "Text recognition: PP-OCRv6 did not load, Windows' instead:" << error;
                    m_paddle = false;
                }
            }
            if (paddle) {
                QStringList lines;
                QString error;
                QImageReader reader(job.file);
                reader.setAutoTransform(true);
                const QImage image = reader.read();
                if (paddle->read(image, &lines, &error)) {
                    words = capture::withQrCodes(lines.join(u'\n'), image); // a code's content with the words (GitHub #305)
                } else {
                    qWarning().noquote() << "Text recognition failed:" << error;
                    ok = false;
                }
                emit done(job.id, words, ok);
                continue;
            }
            try {
                if (!engine && !none) {
                    engine = pickEngine();
                    none = !engine;
                    if (engine)
                        qInfo().noquote() << "Text recognition in" << toQString(engine.RecognizerLanguage().LanguageTag());
                    else
                        qInfo() << "Text recognition: no language with a recognizer";
                }
                if (!engine) {
                    ok = false;
                } else if (const SoftwareBitmap bitmap = loadBitmap(job.file, static_cast<int>(OcrEngine::MaxImageDimension()))) {
                    QStringList lines;
                    for (const OcrLine& line : engine.RecognizeAsync(bitmap).get().Lines()) {
                        QString text = lineText(line);
                        if (!text.isEmpty())
                            lines.append(std::move(text));
                    }
                    QImageReader reader(job.file);
                    reader.setAutoTransform(true);
                    words = capture::withQrCodes(lines.join(u'\n'), reader.read());
                }
            } catch (const winrt::hresult_error& e) {
                // Not marked read: tried again at the next start, rather than
                // every picture left without words for good.
                qWarning().noquote() << "Text recognition failed:" << toQString(e.message());
                ok = false;
            }
            emit done(job.id, words, ok);
        }
    }
    winrt::uninit_apartment();
}

} // namespace ws
