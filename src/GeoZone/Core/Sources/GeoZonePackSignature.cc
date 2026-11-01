/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZonePackSignature.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QRegularExpression>
#include <QtCore/QScopeGuard>

#ifdef GEOZONE_HAVE_OPENSSL
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#endif

namespace GeoZonePackSignature {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("GeoZonePackSignature", text);
}

#ifdef GEOZONE_HAVE_OPENSSL
/// @return The Ed25519 private key of a PEM, nullptr when it isn't one
EVP_PKEY *readPrivateKey(const QByteArray &pem)
{
    BIO *const bio = BIO_new_mem_buf(pem.constData(), static_cast<int>(pem.size()));
    if (!bio) {
        return nullptr;
    }
    EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (key && (EVP_PKEY_id(key) != EVP_PKEY_ED25519)) {
        EVP_PKEY_free(key);
        key = nullptr;
    }
    return key;
}

QByteArray rawPublicKey(EVP_PKEY *key)
{
    QByteArray raw(publicKeySize, Qt::Uninitialized);
    size_t size = static_cast<size_t>(raw.size());
    if ((EVP_PKEY_get_raw_public_key(key, reinterpret_cast<unsigned char*>(raw.data()), &size) != 1) || (size != static_cast<size_t>(publicKeySize))) {
        return QByteArray();
    }
    return raw;
}
#endif

} // namespace

bool available()
{
#ifdef GEOZONE_HAVE_OPENSSL
    return true;
#else
    return false;
#endif
}

QByteArray generatePrivateKey()
{
#ifdef GEOZONE_HAVE_OPENSSL
    EVP_PKEY_CTX *const context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    EVP_PKEY *key = nullptr;
    const auto free = qScopeGuard([&]() { EVP_PKEY_free(key); EVP_PKEY_CTX_free(context); });
    if (!context || (EVP_PKEY_keygen_init(context) != 1) || (EVP_PKEY_keygen(context, &key) != 1)) {
        return QByteArray();
    }
    BIO *const bio = BIO_new(BIO_s_mem());
    if (!bio) {
        return QByteArray();
    }
    QByteArray pem;
    if (PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr) == 1) {
        char *data = nullptr;
        const long size = BIO_get_mem_data(bio, &data);
        pem = QByteArray(data, static_cast<qsizetype>(size));
    }
    BIO_free(bio);
    return pem;
#else
    return QByteArray();
#endif
}

QByteArray publicKey(const QByteArray &privateKeyPem, QString &errorString)
{
#ifdef GEOZONE_HAVE_OPENSSL
    EVP_PKEY *const key = readPrivateKey(privateKeyPem);
    if (!key) {
        errorString = tr("The file is not an Ed25519 private key in PEM format.");
        return QByteArray();
    }
    const QByteArray raw = rawPublicKey(key);
    EVP_PKEY_free(key);
    return raw;
#else
    Q_UNUSED(privateKeyPem);
    errorString = tr("This build of QGroundControl can't sign packs.");
    return QByteArray();
#endif
}

QByteArray sign(const QByteArray &privateKeyPem, const QByteArray &data, QString &errorString)
{
#ifdef GEOZONE_HAVE_OPENSSL
    EVP_PKEY *const key = readPrivateKey(privateKeyPem);
    if (!key) {
        errorString = tr("The file is not an Ed25519 private key in PEM format.");
        return QByteArray();
    }
    EVP_MD_CTX *const context = EVP_MD_CTX_new();
    const auto free = qScopeGuard([&]() { EVP_MD_CTX_free(context); EVP_PKEY_free(key); });
    QByteArray signature(signatureSize, Qt::Uninitialized);
    size_t size = static_cast<size_t>(signature.size());
    // Ed25519 signs the message itself, without a digest of it
    if (!context || (EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key) != 1) ||
        (EVP_DigestSign(context, reinterpret_cast<unsigned char*>(signature.data()), &size, reinterpret_cast<const unsigned char*>(data.constData()),
                        static_cast<size_t>(data.size())) != 1)) {
        errorString = tr("The pack could not be signed.");
        return QByteArray();
    }
    signature.resize(static_cast<qsizetype>(size));
    return signature;
#else
    Q_UNUSED(privateKeyPem);
    Q_UNUSED(data);
    errorString = tr("This build of QGroundControl can't sign packs.");
    return QByteArray();
#endif
}

bool verify(const QByteArray &publicKey, const QByteArray &data, const QByteArray &signature)
{
#ifdef GEOZONE_HAVE_OPENSSL
    if ((publicKey.size() != publicKeySize) || (signature.size() != signatureSize)) {
        return false;
    }
    EVP_PKEY *const key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(publicKey.constData()),
                                                      static_cast<size_t>(publicKey.size()));
    EVP_MD_CTX *const context = EVP_MD_CTX_new();
    const auto free = qScopeGuard([&]() { EVP_MD_CTX_free(context); EVP_PKEY_free(key); });
    return key && context && (EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1) &&
           (EVP_DigestVerify(context, reinterpret_cast<const unsigned char*>(signature.constData()), static_cast<size_t>(signature.size()),
                             reinterpret_cast<const unsigned char*>(data.constData()), static_cast<size_t>(data.size())) == 1);
#else
    Q_UNUSED(publicKey);
    Q_UNUSED(data);
    Q_UNUSED(signature);
    return false;
#endif
}

QByteArray parsePublicKey(const QString &text)
{
    QString base64 = text.trimmed();
    const QString pemBegin = QStringLiteral("-----BEGIN PUBLIC KEY-----");
    const QString pemEnd = QStringLiteral("-----END PUBLIC KEY-----");
    if (base64.startsWith(pemBegin) && base64.endsWith(pemEnd)) {
        base64 = base64.mid(pemBegin.size(), base64.size() - pemBegin.size() - pemEnd.size());
    }
    base64.remove(QRegularExpression(QStringLiteral("\\s")));
    const QByteArray::FromBase64Result decoded = QByteArray::fromBase64Encoding(base64.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) {
        return QByteArray();
    }
    if (decoded.decoded.size() == publicKeySize) {
        return decoded.decoded;
    }
    // The SubjectPublicKeyInfo of an Ed25519 key is this prefix and the raw key (RFC 8410)
    static const QByteArray spkiPrefix = QByteArray::fromHex("302a300506032b6570032100");
    if ((decoded.decoded.size() == (spkiPrefix.size() + publicKeySize)) && decoded.decoded.startsWith(spkiPrefix)) {
        return decoded.decoded.mid(spkiPrefix.size());
    }
    return QByteArray();
}

QString keyId(const QByteArray &publicKey)
{
    const QString hex = QString::fromLatin1(QCryptographicHash::hash(publicKey, QCryptographicHash::Sha256).toHex().left(16));
    return QStringLiteral("%1 %2 %3 %4").arg(hex.mid(0, 4), hex.mid(4, 4), hex.mid(8, 4), hex.mid(12, 4));
}

const Publisher *find(const QList<Publisher> &publishers, const QByteArray &publicKey)
{
    for (const Publisher &publisher : publishers) {
        if (!publicKey.isEmpty() && (publisher.publicKey == publicKey)) {
            return &publisher;
        }
    }
    return nullptr;
}

} // namespace GeoZonePackSignature
