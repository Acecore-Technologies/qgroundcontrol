/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QString>

/// Ed25519 signatures of offline packs, see GeoZonePack. The publisher of a pack signs its manifest, which holds the
/// SHA-256 of each file, with a private key; importers check the signature with the public keys of the publishers
/// they trust. Uses OpenSSL's libcrypto when the build found it (GEOZONE_HAVE_OPENSSL), otherwise packs can't be
/// signed and their signatures can't be checked.
namespace GeoZonePackSignature {

/// A publisher whose packs are trusted
struct Publisher {
    QString name;           ///< Given by the user who trusts it
    QByteArray publicKey;   ///< Raw Ed25519 public key, 32 bytes
};

/// @return true when the build can sign and check signatures
bool available();

/// @return A new Ed25519 private key in PEM (PKCS #8), empty when unavailable
QByteArray generatePrivateKey();
/// @return The raw public key of a PEM private key, empty with errorString set when it isn't an unencrypted
///         Ed25519 key. Encrypted keys aren't supported.
QByteArray publicKey(const QByteArray &privateKeyPem, QString &errorString);
/// @return The signature of the data, empty with errorString set on errors
QByteArray sign(const QByteArray &privateKeyPem, const QByteArray &data, QString &errorString);
/// @return true when the signature of the data is valid for the raw public key
bool verify(const QByteArray &publicKey, const QByteArray &data, const QByteArray &signature);

/// @return The raw public key of a text: its 32 bytes in base64, a SubjectPublicKeyInfo in base64 (DER) or PEM, as
///         openssl pkey -pubout writes it. Empty when it isn't an Ed25519 public key.
QByteArray parsePublicKey(const QString &text);
/// @return Short identifier of a public key shown to users: the first 16 hexadecimal digits of its SHA-256, by 4
QString keyId(const QByteArray &publicKey);

/// @return The publisher with the public key, nullptr when it isn't trusted
const Publisher *find(const QList<Publisher> &publishers, const QByteArray &publicKey);

inline constexpr qsizetype publicKeySize = 32;
inline constexpr qsizetype signatureSize = 64;
inline constexpr const char *algorithm = "Ed25519";

} // namespace GeoZonePackSignature
