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
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtPositioning/QGeoRectangle>

/// Where and how the data of a geozone source was obtained, recorded with each download or load and kept with the
/// source, for briefings and offline packs. Holds no secrets: the URLs are those requested without the credentials of
/// their services.
struct GeoZoneProvenance
{
    Q_DECLARE_TR_FUNCTIONS(GeoZoneProvenance)

public:
    enum class Integrity : quint8 {
        Unchecked,      ///< The publisher gave no SHA-256 of the data
        Verified,       ///< The SHA-256 of the data matches the one the publisher gave
        Failed,         ///< It doesn't, or the one the publisher gave couldn't be read
    };

    QString providerId;         ///< Service template or known service, see GeoZoneService::id and GeoZoneProviders::Service::id
    QString requestUrl;         ///< URL requested, the URL template filled with the area and time, or the local file
    QString finalUrl;           ///< URL which answered after the redirects
    QStringList redirects;      ///< URLs which redirected, in order
    QString etag;
    QString lastModified;
    QDateTime fetchedAt;        ///< When the data was downloaded or the file loaded
    QByteArray sha256;          ///< SHA-256 of the data received, empty for files
    QByteArray contentHash;     ///< BLAKE2b-256 of the stored copy, see GeoZoneParseCache::hash
    qint64 size = -1;           ///< Bytes of the stored copy, negative when unknown
    QGeoRectangle area;         ///< Area of interest downloaded, invalid when the service takes none
    Integrity integrity = Integrity::Unchecked;
    QString integrityText;      ///< Which SHA-256 the data was checked against
    bool imported = false;      ///< The source was imported from an offline pack
    QString packName;           ///< File name of the pack
    QDateTime importedAt;

    bool isEmpty() const { return !fetchedAt.isValid() && requestUrl.isEmpty() && contentHash.isEmpty(); }

    QJsonObject toJson() const;
    static GeoZoneProvenance fromJson(const QJsonObject &object);

    /// @return One line for the list of sources: when and where the data came from, its hash and integrity
    QString summary() const;

    static QString integrityToString(Integrity integrity);

    /// @return The SHA-256 a response's Repr-Digest, Content-Digest or Digest header gives, empty without or when the
    ///         content was encoded, like compressed, since the digests are of the encoded content
    /// @param headers By lower case name
    static QByteArray digestHeaderSha256(const QHash<QByteArray, QByteArray> &headers, QString &headerName);
    /// @return The SHA-256 of a .sha256 file: the hexadecimal hash, alone or followed by the file name like sha256sum
    ///         writes it. Empty when the content isn't one.
    static QByteArray parseSha256File(const QByteArray &content);
};
