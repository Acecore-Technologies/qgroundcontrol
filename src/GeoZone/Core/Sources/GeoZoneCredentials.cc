/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneCredentials.h"
#include "GeoZoneHash.h"

#include <QtCore/QSettings>

GeoZoneCredentials::GeoZoneCredentials(const QString &settingsGroup)
    : _settingsGroup(settingsGroup)
{
}

QString GeoZoneCredentials::_id(const QString &sourceKey)
{
    return QString::fromLatin1(GeoZoneHash::sha256(sourceKey.toUtf8()).toHex());
}

QString GeoZoneCredentials::secret(const QString &sourceKey) const
{
    QSettings settings;
    settings.beginGroup(_settingsGroup);
    return settings.value(_id(sourceKey)).toString();
}

void GeoZoneCredentials::setSecret(const QString &sourceKey, const QString &secret)
{
    QSettings settings;
    settings.beginGroup(_settingsGroup);
    if (secret.isEmpty()) {
        settings.remove(_id(sourceKey));
    } else {
        settings.setValue(_id(sourceKey), secret);
    }
}
