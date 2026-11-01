/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneService.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QUrl>

#include <algorithm>
#include <cmath>

namespace {

constexpr const char *idKey = "id";
constexpr const char *nameKey = "name";
constexpr const char *methodKey = "method";
constexpr const char *authKey = "auth";
constexpr const char *authParameterKey = "authParameter";
constexpr const char *userKey = "user";
constexpr const char *tokenUrlKey = "tokenUrl";
constexpr const char *scopeKey = "scope";
constexpr const char *headersKey = "headers";
constexpr const char *responseMemberKey = "responseMember";
constexpr const char *areaOfInterestKey = "areaOfInterest";
constexpr const char *timeWindowKey = "timeWindowHours";
constexpr const char *sha256UrlKey = "sha256Url";

const QStringList kAreaPlaceholders = {
    QStringLiteral("{west}"), QStringLiteral("{south}"), QStringLiteral("{east}"), QStringLiteral("{north}"), QStringLiteral("{bbox}"),
    QStringLiteral("{lat}"), QStringLiteral("{lon}"), QStringLiteral("{radiusM}"), QStringLiteral("{radiusKm}"), QStringLiteral("{radiusNm}"),
};

QString isoTime(const QDateTime &time)
{
    return time.toUTC().toString(Qt::ISODate);
}

QString degrees(double value)
{
    return QString::number(value, 'f', 5);
}

} // namespace

bool GeoZoneService::isEmpty() const
{
    return *this == GeoZoneService();
}

QString GeoZoneService::effectiveResponseMember() const
{
    return (responseMember.isEmpty() && (method == Method::Ed318Post)) ? QStringLiteral("uasZones") : responseMember;
}

QString GeoZoneService::methodToString(Method method)
{
    return (method == Method::Ed318Post) ? QStringLiteral("ed318Post") : QStringLiteral("get");
}

QString GeoZoneService::authToString(Auth auth)
{
    switch (auth) {
    case Auth::None: return QStringLiteral("none");
    case Auth::ApiKeyHeader: return QStringLiteral("apiKeyHeader");
    case Auth::ApiKeyQuery: return QStringLiteral("apiKeyQuery");
    case Auth::Bearer: return QStringLiteral("bearer");
    case Auth::Basic: return QStringLiteral("basic");
    case Auth::OAuth2ClientCredentials: return QStringLiteral("oauth2ClientCredentials");
    }
    return QStringLiteral("none");
}

QJsonObject GeoZoneService::toJson() const
{
    QJsonObject object;
    const auto insert = [&object](const char *key, const QString &value) {
        if (!value.isEmpty()) {
            object.insert(QLatin1String(key), value);
        }
    };
    insert(idKey, id);
    insert(nameKey, name);
    object.insert(QLatin1String(methodKey), methodToString(method));
    object.insert(QLatin1String(authKey), authToString(auth));
    insert(authParameterKey, authParameter);
    insert(userKey, user);
    insert(tokenUrlKey, tokenUrl);
    insert(scopeKey, scope);
    if (!headers.isEmpty()) {
        QJsonObject headerObject;
        for (const auto &[headerName, value] : headers) {
            headerObject.insert(headerName, value);
        }
        object.insert(QLatin1String(headersKey), headerObject);
    }
    insert(responseMemberKey, responseMember);
    object.insert(QLatin1String(areaOfInterestKey), areaOfInterest);
    object.insert(QLatin1String(timeWindowKey), timeWindowHours);
    insert(sha256UrlKey, sha256Url);
    return object;
}

GeoZoneService GeoZoneService::fromJson(const QJsonObject &object)
{
    GeoZoneService service;
    service.id = object.value(QLatin1String(idKey)).toString();
    service.name = object.value(QLatin1String(nameKey)).toString();
    service.method = (object.value(QLatin1String(methodKey)).toString() == methodToString(Method::Ed318Post)) ? Method::Ed318Post : Method::Get;
    const QString auth = object.value(QLatin1String(authKey)).toString();
    for (const Auth candidate : { Auth::ApiKeyHeader, Auth::ApiKeyQuery, Auth::Bearer, Auth::Basic, Auth::OAuth2ClientCredentials }) {
        if (auth == authToString(candidate)) {
            service.auth = candidate;
        }
    }
    service.authParameter = object.value(QLatin1String(authParameterKey)).toString().trimmed();
    service.user = object.value(QLatin1String(userKey)).toString().trimmed();
    service.tokenUrl = object.value(QLatin1String(tokenUrlKey)).toString().trimmed();
    service.scope = object.value(QLatin1String(scopeKey)).toString().trimmed();
    const QJsonObject headerObject = object.value(QLatin1String(headersKey)).toObject();
    for (auto it = headerObject.constBegin(); it != headerObject.constEnd(); ++it) {
        service.headers.append({ it.key(), it.value().toString() });
    }
    service.responseMember = object.value(QLatin1String(responseMemberKey)).toString().trimmed();
    service.areaOfInterest = object.value(QLatin1String(areaOfInterestKey)).toBool();
    service.timeWindowHours = std::clamp(object.value(QLatin1String(timeWindowKey)).toInt(24), 1, 24 * 31);
    service.sha256Url = object.value(QLatin1String(sha256UrlKey)).toString().trimmed();
    return service;
}

bool GeoZoneService::hasAreaPlaceholders(const QString &urlTemplate)
{
    return std::any_of(kAreaPlaceholders.cbegin(), kAreaPlaceholders.cend(), [&urlTemplate](const QString &placeholder) {
        return urlTemplate.contains(placeholder) || urlTemplate.contains(QString(placeholder).replace(QLatin1Char('{'), QStringLiteral("%7B")).replace(QLatin1Char('}'), QStringLiteral("%7D")), Qt::CaseInsensitive);
    });
}

QString GeoZoneService::resolve(const QString &urlTemplate, const QGeoRectangle &area, const QDateTime &start, const QDateTime &end)
{
    // Templates entered in a URL field may have their braces percent-encoded
    QString url = urlTemplate;
    (void) url.replace(QStringLiteral("%7B"), QStringLiteral("{"), Qt::CaseInsensitive).replace(QStringLiteral("%7D"), QStringLiteral("}"), Qt::CaseInsensitive);

    if (area.isValid()) {
        const QGeoCoordinate center = area.center();
        const double radius = (std::max)(center.distanceTo(area.topLeft()), center.distanceTo(area.bottomRight()));
        const double west = area.topLeft().longitude();
        const double north = area.topLeft().latitude();
        const double east = area.bottomRight().longitude();
        const double south = area.bottomRight().latitude();
        (void) url.replace(QStringLiteral("{bbox}"), QStringLiteral("%1,%2,%3,%4").arg(degrees(west), degrees(south), degrees(east), degrees(north)));
        (void) url.replace(QStringLiteral("{west}"), degrees(west)).replace(QStringLiteral("{south}"), degrees(south));
        (void) url.replace(QStringLiteral("{east}"), degrees(east)).replace(QStringLiteral("{north}"), degrees(north));
        (void) url.replace(QStringLiteral("{lat}"), degrees(center.latitude())).replace(QStringLiteral("{lon}"), degrees(center.longitude()));
        (void) url.replace(QStringLiteral("{radiusM}"), QString::number(std::ceil(radius), 'f', 0));
        (void) url.replace(QStringLiteral("{radiusKm}"), QString::number(std::ceil(radius / 100.0) / 10.0, 'f', 1));
        (void) url.replace(QStringLiteral("{radiusNm}"), QString::number(std::ceil(radius / 185.2) / 10.0, 'f', 1));
    }
    if (start.isValid()) {
        (void) url.replace(QStringLiteral("{start}"), QString::fromLatin1(QUrl::toPercentEncoding(isoTime(start))));
    }
    if (end.isValid()) {
        (void) url.replace(QStringLiteral("{end}"), QString::fromLatin1(QUrl::toPercentEncoding(isoTime(end))));
    }
    return url;
}

QByteArray GeoZoneService::ed318Request(const QGeoRectangle &area, const QDateTime &start, const QDateTime &end)
{
    QJsonObject request;
    if (area.isValid()) {
        const double west = area.topLeft().longitude();
        const double north = area.topLeft().latitude();
        const double east = area.bottomRight().longitude();
        const double south = area.bottomRight().latitude();
        const QJsonArray ring = { QJsonArray{ west, south }, QJsonArray{ east, south }, QJsonArray{ east, north }, QJsonArray{ west, north }, QJsonArray{ west, south } };
        request.insert(QStringLiteral("geometry"), QJsonObject{ { QStringLiteral("type"), QStringLiteral("Polygon") }, { QStringLiteral("coordinates"), QJsonArray{ ring } } });
    }
    if (start.isValid()) {
        request.insert(QStringLiteral("startDateTime"), isoTime(start));
    }
    if (end.isValid()) {
        request.insert(QStringLiteral("endDateTime"), isoTime(end));
    }
    return QJsonDocument(request).toJson(QJsonDocument::Compact);
}

QString GeoZoneService::requestUrl(const QString &urlTemplate, const QGeoRectangle &area, const QDateTime &start) const
{
    return resolve(urlTemplate, area, start, start.addSecs(qint64{ timeWindowHours } * 3600));
}

GeoZoneDownloader::Options GeoZoneService::downloadOptions(const QGeoRectangle &area, const QDateTime &start, const QString &secret, const QString &accessToken) const
{
    GeoZoneDownloader::Options options;
    for (const auto &[headerName, value] : headers) {
        options.headers.append({ headerName.toUtf8(), value.toUtf8() });
    }
    if (method == Method::Ed318Post) {
        options.post = true;
        options.contentType = "application/json";
        options.body = ed318Request(area, start, start.addSecs(qint64{ timeWindowHours } * 3600));
        options.headers.append(GeoZoneDownloader::Header("Accept", "application/json"));
    }

    switch (auth) {
    case Auth::None:
        break;
    case Auth::ApiKeyHeader:
        options.authHeaders.append({ authParameter.toUtf8(), secret.toUtf8() });
        break;
    case Auth::ApiKeyQuery:
        options.authQuery.append({ authParameter, secret });
        break;
    case Auth::Bearer:
        options.authHeaders.append({ "Authorization", "Bearer " + secret.toUtf8() });
        break;
    case Auth::Basic: {
        const QByteArray credentials = (user + QLatin1Char(':') + secret).toUtf8().toBase64();
        options.authHeaders.append({ "Authorization", "Basic " + credentials });
        options.secrets.append(QString::fromLatin1(credentials));
        break;
    }
    case Auth::OAuth2ClientCredentials:
        options.authHeaders.append({ "Authorization", "Bearer " + accessToken.toUtf8() });
        options.secrets.append(accessToken);
        break;
    }
    options.secrets.append(secret);
    return options;
}

GeoZoneDownloader::Options GeoZoneService::tokenRequestOptions(const QString &secret) const
{
    // RFC 6749 client credentials grant, the client authenticated with basic authentication
    GeoZoneDownloader::Options options;
    options.post = true;
    options.contentType = "application/x-www-form-urlencoded";
    options.body = "grant_type=client_credentials";
    if (!scope.isEmpty()) {
        options.body += "&scope=" + QUrl::toPercentEncoding(scope);
    }
    options.headers.append(GeoZoneDownloader::Header("Accept", "application/json"));
    const QByteArray credentials = (QString::fromLatin1(QUrl::toPercentEncoding(user)) + QLatin1Char(':') + QString::fromLatin1(QUrl::toPercentEncoding(secret))).toUtf8().toBase64();
    options.authHeaders.append({ "Authorization", "Basic " + credentials });
    options.secrets = { secret, QString::fromLatin1(credentials) };
    return options;
}

QByteArray GeoZoneService::extractMember(const QByteArray &response, const QString &member, QString &errorString)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        errorString = tr("The response of the service is not JSON: %1").arg(parseError.errorString());
        return QByteArray();
    }
    QJsonValue value = document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object());
    for (const QString &name : member.split(QLatin1Char('.'), Qt::SkipEmptyParts)) {
        value = value.toObject().value(name);
    }
    if (value.isArray()) {
        return QJsonDocument(QJsonObject{ { QStringLiteral("type"), QStringLiteral("FeatureCollection") }, { QStringLiteral("features"), value.toArray() } })
            .toJson(QJsonDocument::Compact);
    }
    if (value.isObject()) {
        return QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    }
    errorString = tr("The response of the service has no %1.").arg(member);
    return QByteArray();
}
