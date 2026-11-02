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

/// The secrets of the geozone services by source: API keys, tokens, passwords and OAuth2 client secrets. They are kept
/// unencrypted in a group of the application settings (QSettings) under the SHA-256 of the source key, which doesn't
/// reveal the source, apart from the index of the sources and from exported packs, and are never logged.
class GeoZoneCredentials
{
public:
    explicit GeoZoneCredentials(const QString &settingsGroup = QStringLiteral("GeoZoneCredentials"));

    QString secret(const QString &sourceKey) const;
    /// Keeps the secret of a source, removes it when empty
    void setSecret(const QString &sourceKey, const QString &secret);
    void remove(const QString &sourceKey) { setSecret(sourceKey, QString()); }

private:
    /// @return Key of the secret of a source in the settings, its key hashed since keys are URLs
    static QString _id(const QString &sourceKey);

    QString _settingsGroup;
};
