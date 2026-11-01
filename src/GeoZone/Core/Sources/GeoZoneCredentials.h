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

#include <memory>

#include "GeoZoneCredentialStore.h"

/// The secrets of the geozone services by source: API keys, tokens, passwords and OAuth2 client secrets. They are kept
/// in the GeoZoneCredentialStore installed by the application, a secure one like the system keychain when it has one,
/// otherwise in the application settings, unencrypted. They are kept apart from the index of the sources and from
/// exported packs, and are never logged.
class GeoZoneCredentials
{
public:
    /// @param settingsGroup Group of the application settings keeping the secrets without a store, or when it fails
    explicit GeoZoneCredentials(const QString &settingsGroup = QString::fromLatin1(defaultSettingsGroup));

    /// @return The secret of the source, from the settings when the store couldn't keep it
    QString secret(const QString &sourceKey) const;
    /// Keeps the secret of a source, removes it when empty. A secret the store can't keep is kept in the settings,
    /// a secret it keeps is removed from them.
    void setSecret(const QString &sourceKey, const QString &secret);
    void remove(const QString &sourceKey) { setSecret(sourceKey, QString()); }

    /// Installs the store keeping the secrets of all sources, and moves the secrets kept in the settings into it. Not
    /// thread safe: called at startup by the application, or by tests.
    /// @param store Replaces the store installed, nullptr keeps the secrets in the settings
    /// @return Number of secrets moved from the settings
    static int setStore(std::unique_ptr<GeoZoneCredentialStore> store);
    /// @return Name of the store keeping the secrets, see GeoZoneCredentialStore::name
    static QString storeName();
    /// @return true when the secrets are kept encrypted by the system
    static bool storeIsSecure();

    static constexpr const char *defaultSettingsGroup = GeoZoneSettingsCredentialStore::defaultSettingsGroup;

private:
    /// @return Id of the secret of a source in the stores, its key hashed since keys are URLs
    static QString _id(const QString &sourceKey);
    /// @return The store installed, null without
    static GeoZoneCredentialStore *_store();

    GeoZoneSettingsCredentialStore _settings;
};
