/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneCredentialStore.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QSettings>

GeoZoneSettingsCredentialStore::GeoZoneSettingsCredentialStore(const QString &settingsGroup)
    : _settingsGroup(settingsGroup)
{
}

QString GeoZoneSettingsCredentialStore::name() const
{
    return QCoreApplication::translate("GeoZoneCredentials", "the application settings, unencrypted");
}

QString GeoZoneSettingsCredentialStore::secret(const QString &id) const
{
    QSettings settings;
    settings.beginGroup(_settingsGroup);
    return settings.value(id).toString();
}

bool GeoZoneSettingsCredentialStore::setSecret(const QString &id, const QString &secret)
{
    QSettings settings;
    settings.beginGroup(_settingsGroup);
    if (secret.isEmpty()) {
        settings.remove(id);
    } else {
        settings.setValue(id, secret);
    }
    settings.sync();
    return settings.status() == QSettings::NoError;
}

QStringList GeoZoneSettingsCredentialStore::ids() const
{
    QSettings settings;
    settings.beginGroup(_settingsGroup);
    return settings.childKeys();
}
