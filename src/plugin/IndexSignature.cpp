#include "IndexSignature.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <windows.h>
#include <bcrypt.h>

#include <string_view>

using namespace Qt::StringLiterals;

namespace ws::plugin::signing {

namespace {

// Same as WinShun-plugins/keys/<id>.pub; rotation: WinShun-plugins/keys/README.md
constexpr struct {
    std::u16string_view id;
    std::string_view xy;
} kIndexKeys[] = {
    {u"ws-index-2026a", "09148082336ebf8e95fdb36a163f0bf8a11b58284e2bd5ff8e93b9905d56ebd04034298bb42868797407dcc5f14368ad304ea82025f04750d81fc5717bf3f857"},
};

constexpr qsizetype kCoordinateBytes = 32;

bool succeeded(NTSTATUS status) { return status == 0; } // STATUS_SUCCESS

// The key as CNG takes it, in `key`; close the algorithm with `alg`.
bool importKey(const QByteArray& xy, BCRYPT_ALG_HANDLE* alg, BCRYPT_KEY_HANDLE* key)
{
    if (xy.size() != 2 * kCoordinateBytes)
        return false;
    if (!succeeded(::BCryptOpenAlgorithmProvider(alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)))
        return false;
    QByteArray blob(static_cast<qsizetype>(sizeof(BCRYPT_ECCKEY_BLOB)), '\0');
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header->cbKey = kCoordinateBytes;
    blob.append(xy);
    if (succeeded(::BCryptImportKeyPair(*alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, key, reinterpret_cast<PUCHAR>(blob.data()),
            static_cast<ULONG>(blob.size()), 0)))
        return true;
    ::BCryptCloseAlgorithmProvider(*alg, 0);
    return false;
}

} // namespace

const QList<PublicKey>& indexKeys()
{
    static const QList<PublicKey> keys = [] {
        QList<PublicKey> list;
        for (const auto& k : kIndexKeys)
            list.push_back({QString::fromUtf16(k.id.data(), static_cast<qsizetype>(k.id.size())),
                QByteArray::fromHex(QByteArray(k.xy.data(), static_cast<qsizetype>(k.xy.size())))});
        return list;
    }();
    return keys;
}

bool importable(const QByteArray& xy)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!importKey(xy, &alg, &key))
        return false;
    ::BCryptDestroyKey(key);
    ::BCryptCloseAlgorithmProvider(alg, 0);
    return true;
}

std::optional<QList<Signature>> parseFile(const QByteArray& sigFile, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error)
            *error = why;
        return std::nullopt;
    };
    if (sigFile.size() > kMaxSignatureBytes)
        return fail(u"签名文件太大"_s);
    const QJsonDocument document = QJsonDocument::fromJson(sigFile);
    if (!document.isObject())
        return fail(u"签名文件不是 JSON"_s);
    const QJsonObject root = document.object();
    if (root.value(u"schema"_s).toInt() != 1)
        return fail(u"不认识的签名文件 schema"_s);
    const QJsonArray entries = root.value(u"signatures"_s).toArray();
    if (entries.isEmpty() || entries.size() > 4)
        return fail(u"签名文件里要有 1 到 4 项签名"_s);
    static const QRegularExpression keyId(u"^[a-z0-9-]{1,40}$"_s);
    QList<Signature> list;
    for (const QJsonValue value : entries) {
        const QJsonObject o = value.toObject();
        if (o.value(u"algorithm"_s).toString() != u"ecdsa-p256-sha256"_s)
            continue; // another algorithm: for later
        const QString key = o.value(u"key"_s).toString();
        if (!keyId.match(key).hasMatch())
            continue;
        const auto decoded = QByteArray::fromBase64Encoding(o.value(u"signature"_s).toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.size() != 2 * kCoordinateBytes)
            continue;
        list.push_back({key, decoded.decoded});
    }
    if (list.isEmpty())
        return fail(u"签名文件里没有能用的签名"_s);
    return list;
}

bool verifyP256(const QByteArray& data, const QByteArray& rs, const QByteArray& xy)
{
    if (rs.size() != 2 * kCoordinateBytes)
        return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!importKey(xy, &alg, &key))
        return false;
    UCHAR hash[32] = {};
    bool ok = succeeded(::BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.constData())),
                    static_cast<ULONG>(data.size()), hash, sizeof(hash)))
        && succeeded(::BCryptVerifySignature(key, nullptr, hash, sizeof(hash), reinterpret_cast<PUCHAR>(const_cast<char*>(rs.constData())),
            static_cast<ULONG>(rs.size()), 0));
    ::BCryptDestroyKey(key);
    ::BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

Verdict verify(const QByteArray& index, const QByteArray& sigFile, const QList<PublicKey>& keys)
{
    Verdict verdict;
    QString error;
    const std::optional<QList<Signature>> signatures = parseFile(sigFile, &error);
    if (!signatures) {
        verdict.why = error;
        return verdict;
    }
    bool known = false;
    for (const Signature& s : *signatures) {
        for (const PublicKey& k : keys) {
            if (k.id != s.key)
                continue;
            known = true;
            if (verifyP256(index, s.rs, k.xy)) {
                verdict.ok = true;
                verdict.key = k.id;
                return verdict;
            }
        }
    }
    verdict.why = known ? u"签名对不上"_s : u"签名用的钥匙这个版本不认识"_s;
    return verdict;
}

} // namespace ws::plugin::signing
