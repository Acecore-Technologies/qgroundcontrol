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
#include <QtCore/QStringList>

/// Where the secrets of the geozone services are kept, see GeoZoneCredentials. Secrets are identified by an id
/// which doesn't reveal the source, the SHA-256 of its key. Stores are used from the GUI thread.
///
/// QGroundControl has no secure store on all of its platforms, so GeoZoneSettingsCredentialStore, which keeps the
/// secrets unencrypted in the application settings, is the default and the fallback. A store backed by the system
/// keychain (like QtKeychain: the Secret Service through libsecret on Linux, the Keychain on macOS and iOS, the
/// Credential Manager on Windows and the Keystore on Android) implements this interface and is installed with
/// GeoZoneCredentials::setStore().
class GeoZoneCredentialStore
{
public:
    virtual ~GeoZoneCredentialStore() = default;

    /// @return Name of the store shown to the user, like "the system keychain"
    virtual QString name() const = 0;
    /// @return true when the system encrypts the secrets, false when they are kept in plain text
    virtual bool isSecure() const = 0;
    /// @return The secret with the id, empty when there is none or it can't be read
    virtual QString secret(const QString &id) const = 0;
    /// Keeps a secret, removes it when empty
    /// @return false when the store failed
    virtual bool setSecret(const QString &id, const QString &secret) = 0;
    /// @return Ids of the secrets kept, empty when the store can't list them
    virtual QStringList ids() const = 0;
};

/// Keeps the secrets unencrypted in a group of the application settings (QSettings), apart from the index of the
/// sources and from exported packs
class GeoZoneSettingsCredentialStore : public GeoZoneCredentialStore
{
public:
    explicit GeoZoneSettingsCredentialStore(const QString &settingsGroup = QString::fromLatin1(defaultSettingsGroup));

    QString name() const override;
    bool isSecure() const override { return false; }
    QString secret(const QString &id) const override;
    bool setSecret(const QString &id, const QString &secret) override;
    QStringList ids() const override;

    static constexpr const char *defaultSettingsGroup = "GeoZoneCredentials";

private:
    QString _settingsGroup;
};
