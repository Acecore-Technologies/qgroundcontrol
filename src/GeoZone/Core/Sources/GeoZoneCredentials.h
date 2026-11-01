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

/// The secrets of the geozone services by source: API keys, tokens, passwords and OAuth2 client secrets.
/// QGroundControl has no secure store on all of its platforms, so they are kept in the application settings,
/// unencrypted, apart from the index of the sources and from exported packs, and are never logged.
class GeoZoneCredentials
{
public:
    /// @param settingsGroup Group of the application settings keeping the secrets
    explicit GeoZoneCredentials(const QString &settingsGroup = QString::fromLatin1(defaultSettingsGroup));

    QString secret(const QString &sourceKey) const;
    /// Keeps the secret of a source, removes it when empty
    void setSecret(const QString &sourceKey, const QString &secret);
    void remove(const QString &sourceKey) { setSecret(sourceKey, QString()); }

    static constexpr const char *defaultSettingsGroup = "GeoZoneCredentials";

private:
    /// @return Settings key of a source, its key hashed since keys are URLs
    static QString _settingsKey(const QString &sourceKey);

    QString _settingsGroup;
};
