#pragma once

// The plugin list's signature (the plugin updates design, §2): the
// WinShun-plugins repository publishes index.json.sig beside index.json, an
// ECDSA P-256 / SHA-256 signature (IEEE P1363: r||s, 64 bytes, in Base64 in a
// small JSON file) over the list's exact bytes. Win顺 carries the public keys
// and checks with Windows' own CNG (bcrypt.dll). Only a list that checks may
// be installed from without asking; a mirror may carry both files.

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace ws::plugin::signing {

inline constexpr qint64 kMaxSignatureBytes = 4096;

struct PublicKey {
    QString id;
    QByteArray xy; // 64 bytes: X || Y (the uncompressed point without its 0x04)
};
// The keys built in (IndexSignature.cpp); the list's signature is checked with these.
const QList<PublicKey>& indexKeys();
// The point is a public key CNG takes (on the curve).
bool importable(const QByteArray& xy);

struct Signature {
    QString key; // the id of the key that signed
    QByteArray rs; // 64 bytes: r || s, 32 each, big-endian
};
// The signatures of an index.json.sig that Win顺 can use: ones of another
// algorithm, and ones whose Base64 or length is wrong, are left out. Nothing,
// with `error`, when the file is no signature file or has none left.
std::optional<QList<Signature>> parseFile(const QByteArray& sigFile, QString* error);

// ECDSA P-256 / SHA-256 of `data` against `rs` and the public key `xy`.
bool verifyP256(const QByteArray& data, const QByteArray& rs, const QByteArray& xy);

struct Verdict {
    bool ok = false;
    QString key; // the key that checked
    QString why; // when not ok
};
// `index` (the list's bytes as fetched) against `sigFile`: ok when one of its
// signatures is by one of `keys` and checks.
Verdict verify(const QByteArray& index, const QByteArray& sigFile, const QList<PublicKey>& keys);

} // namespace ws::plugin::signing
