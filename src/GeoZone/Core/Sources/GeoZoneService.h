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
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QVariantMap>
#include <QtPositioning/QGeoRectangle>

#include "GeoZoneDownloader.h"

/// How the zones of a feed are requested from the service publishing them: the URL template filled with the area and
/// time of interest, the request method, the authentication, the member of the response holding the zones and where
/// the service publishes the SHA-256 of its data. Holds no secrets, GeoZoneCredentials keeps them. Feeds without a
/// service are plain GET requests of their URL.
///
/// URL templates take the placeholders {west}, {south}, {east}, {north} and {bbox} (west,south,east,north) of the area
/// in degrees, {lat}, {lon} of its center, {radiusM}, {radiusKm} and {radiusNm} of the circle around it, and {start}
/// and {end} of the time of interest (ISO 8601 UTC). The SHA-256 URL template also takes {url}, the URL requested.
struct GeoZoneService
{
    Q_DECLARE_TR_FUNCTIONS(GeoZoneService)

public:
    enum class Method : quint8 {
        Get,
        Ed318Post,      ///< POST of an ED-318 UASZonesRequest with the area as polygon and the time of interest
    };

    enum class Auth : quint8 {
        None,
        ApiKeyHeader,   ///< The secret in the header named authParameter
        ApiKeyQuery,    ///< The secret in the query parameter named authParameter
        Bearer,         ///< The secret is a bearer token
        Basic,          ///< HTTP basic authentication of user with the secret as password
        OAuth2ClientCredentials,    ///< Bearer tokens from tokenUrl for the client user, with the secret as client secret
    };

    QString id;                 ///< Template the service was made from, see GeoZoneProviders::serviceTemplates, empty for others
    QString name;               ///< Shown instead of the title of the data or the host when not empty
    Method method = Method::Get;
    Auth auth = Auth::None;
    QString authParameter;      ///< Header or query parameter of an API key
    QString user;               ///< User of basic authentication, or OAuth2 client id
    QString tokenUrl;           ///< OAuth2 token endpoint
    QString scope;              ///< OAuth2 scope, optional
    QList<std::pair<QString, QString>> headers;     ///< Further request headers, like the response format
    QString responseMember;     ///< Member of the JSON response holding the zones, dot separated, like "data.geojson"
    bool areaOfInterest = false;    ///< The request takes the area of interest, the zones are downloaded again when it moves out
    int timeWindowHours = 24;   ///< Time of interest after the request
    double maxAreaKm2 = 0;      ///< Largest area the service answers, the margin around the area of interest shrinks to fit. 0 without limit.
    QString sha256Url;          ///< Template of the URL publishing the SHA-256 of the data, empty when not published

    bool isEmpty() const;
    bool needsSecret() const { return auth != Auth::None; }
    /// @return responseMember, or the member of the zones in ED-318 replies when the request is an ED-318 POST
    QString effectiveResponseMember() const;
    bool operator==(const GeoZoneService &other) const = default;

    /// @return Without the secrets, which it never holds
    QJsonObject toJson() const;
    static GeoZoneService fromJson(const QJsonObject &object);
    /// For QML: the fields of toJson with the enums as their names
    QVariantMap toVariantMap() const { return toJson().toVariantMap(); }
    static GeoZoneService fromVariantMap(const QVariantMap &map) { return fromJson(QJsonObject::fromVariantMap(map)); }

    /// @return true when the URL template has placeholders of the area
    static bool hasAreaPlaceholders(const QString &urlTemplate);
    /// @return Approximate surface of the area in km²
    static double areaKm2(const QGeoRectangle &area);
    /// @return true when the service answers the area
    bool acceptsArea(const QGeoRectangle &area) const { return (maxAreaKm2 <= 0) || (areaKm2(area) <= maxAreaKm2); }
    /// @return The URL template with the placeholders filled
    static QString resolve(const QString &urlTemplate, const QGeoRectangle &area, const QDateTime &start, const QDateTime &end);
    /// @return ED-318 UASZonesRequest of the zones in the area during the time
    static QByteArray ed318Request(const QGeoRectangle &area, const QDateTime &start, const QDateTime &end);

    /// @return URL to request for the feed's URL template, the area and the time of interest starting at start
    QString requestUrl(const QString &urlTemplate, const QGeoRectangle &area, const QDateTime &start) const;
    /// @return How to download the feed: headers, credentials, query and the secrets to keep out of messages
    /// @param accessToken OAuth2 token
    GeoZoneDownloader::Options downloadOptions(const QGeoRectangle &area, const QDateTime &start, const QString &secret,
                                               const QString &accessToken = QString()) const;
    /// @return How to request an OAuth2 token with the client secret
    GeoZoneDownloader::Options tokenRequestOptions(const QString &secret) const;

    /// @return The JSON document of the response member holding the zones: an object, or a feature collection of an
    ///         array. Empty when the response isn't JSON or lacks the member.
    static QByteArray extractMember(const QByteArray &response, const QString &member, QString &errorString);

    static QString methodToString(Method method);
    static QString authToString(Auth auth);
};
