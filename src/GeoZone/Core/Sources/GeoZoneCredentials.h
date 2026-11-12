/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QString>

/// The secrets of the geozone services by source: API keys, tokens, passwords and OAuth2 client secrets. They are kept
/// unencrypted in a file of their own in the application data location, "<name>.json", which only its owner can read
/// and write: the file is made with these permissions before a secret is written to it. Each secret is under the
/// SHA-256 of its source key, which doesn't reveal the source. They are kept apart from the application settings,
/// which are shared and exported more readily, from the index of the sources and from exported packs, and are never
/// logged.
///
/// Earlier versions kept them in a group of the application settings (QSettings) named like the file: those are moved
/// to the file, and removed from the settings, when the credentials are made. The credentials have no state of their
/// own, all those of a name read and write the same file.
class GeoZoneCredentials
{
public:
    /// @param name Base name of the file, and the group of the settings of earlier versions
    explicit GeoZoneCredentials(const QString &name = QStringLiteral("GeoZoneCredentials"));

    QString secret(const QString &sourceKey) const;
    /// Keeps the secret of a source, removes it when empty
    /// @return false when the file couldn't be written, or is of a newer version: the secret then isn't kept
    bool setSecret(const QString &sourceKey, const QString &secret);
    bool remove(const QString &sourceKey) { return setSecret(sourceKey, QString()); }

    /// @return Path of the file of the secrets
    QString filePath() const;

    static constexpr int fileVersion = 1;

private:
    /// @return Key of the secret of a source in the file, its key hashed since keys are URLs
    static QString _id(const QString &sourceKey);
    /// @return The secrets by _id, empty when there are none or the file can't be read
    /// @param newer Set to whether the file is of a newer version, which is neither read nor replaced
    QJsonObject _read(bool &newer) const;
    /// Writes the secrets to a file only its owner can read, removes the file without secrets
    bool _write(const QJsonObject &secrets) const;
    /// Moves the secrets of the settings of earlier versions to the file
    void _migrate() const;

    QString _name;
};
