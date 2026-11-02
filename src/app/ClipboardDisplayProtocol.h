#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

namespace ws::clipdisplay {

// Private, process-local UI channel. No clipboard data is stored here.
constexpr quint32 kMaxFrame = 1u << 20;
constexpr int kRowsPerFrame = 128;

inline QByteArray frame(const QJsonObject& object)
{
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (payload.size() > kMaxFrame)
        return {};
    QByteArray bytes(4, Qt::Uninitialized);
    qToLittleEndian<quint32>(quint32(payload.size()), bytes.data());
    bytes += payload;
    return bytes;
}

class Decoder {
public:
    bool append(QByteArrayView bytes)
    {
        if (m_failed)
            return false;
        if (bytes.size() > 3 * kMaxFrame || m_bytes.size() + bytes.size() > 3 * kMaxFrame) {
            m_failed = true;
            return false;
        }
        m_bytes.append(bytes.data(), bytes.size());
        if (m_bytes.size() >= 4 && qFromLittleEndian<quint32>(m_bytes.constData()) > kMaxFrame)
            m_failed = true;
        return !m_failed;
    }

    bool take(QJsonObject& object)
    {
        if (m_failed || m_bytes.size() < 4)
            return false;
        const quint32 size = qFromLittleEndian<quint32>(m_bytes.constData());
        if (size > kMaxFrame) {
            m_failed = true;
            return false;
        }
        if (m_bytes.size() < size + 4)
            return false;
        QJsonParseError error;
        const auto json = QJsonDocument::fromJson(m_bytes.mid(4, size), &error);
        m_bytes.remove(0, size + 4);
        if (error.error != QJsonParseError::NoError || !json.isObject()) {
            m_failed = true;
            return false;
        }
        object = json.object();
        return true;
    }

    bool failed() const { return m_failed; }

private:
    QByteArray m_bytes;
    bool m_failed = false;
};

} // namespace ws::clipdisplay
