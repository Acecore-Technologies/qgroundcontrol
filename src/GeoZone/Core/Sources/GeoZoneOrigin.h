/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QString>
#include <QtCore/QUrl>

/// Origins of URLs: their scheme, host and port. The credentials and the custom headers of a geozone service are only
/// sent to the origin of the feed they were configured for, see GeoZoneDownloader and GeoZoneFeedFetch.
namespace GeoZoneOrigin {

/// @return Port of the URL, the default port of http and https when it has none, -1 for other schemes without
inline int port(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    return url.port((scheme == QStringLiteral("https")) ? 443 : ((scheme == QStringLiteral("http")) ? 80 : -1));
}

/// @return true when both URLs are valid, have a host and the same scheme, host and port
inline bool same(const QUrl &a, const QUrl &b)
{
    return a.isValid() && b.isValid() && !a.host().isEmpty() && (a.scheme().compare(b.scheme(), Qt::CaseInsensitive) == 0) &&
           (a.host().compare(b.host(), Qt::CaseInsensitive) == 0) && (port(a) == port(b));
}

/// @return The scheme, host and port of the URL, like https://example.com:8443, empty when it has no host
inline QString name(const QUrl &url)
{
    if (!url.isValid() || url.host().isEmpty()) {
        return QString();
    }
    return url.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo).toString();
}

} // namespace GeoZoneOrigin
