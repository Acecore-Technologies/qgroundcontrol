/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneCredentials.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QSettings>

GeoZoneCredentials::GeoZoneCredentials(const QString &settingsGroup)
    : _settingsGroup(settingsGroup)
{
}

QString GeoZoneCredentials::_settingsKey(const QString &sourceKey)
{
    return QString::fromLatin1(QCryptographicHash::hash(sourceKey.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString GeoZoneCredentials::secret(const QString &sourceKey) const
{
    QSettings settings;
    settings.beginGroup(_settingsGroup);
    return settings.value(_settingsKey(sourceKey)).toString();
}

void GeoZoneCredentials::setSecret(const QString &sourceKey, const QString &secret)
{
    QSettings settings;
    settings.beginGroup(_settingsGroup);
    if (secret.isEmpty()) {
        settings.remove(_settingsKey(sourceKey));
    } else {
        settings.setValue(_settingsKey(sourceKey), secret);
    }
}
