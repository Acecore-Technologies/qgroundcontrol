/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneCredentials.h"
#include "GeoZoneSource.h"

#include <QtCore/QCryptographicHash>

namespace {

std::unique_ptr<GeoZoneCredentialStore> &installedStore()
{
    static std::unique_ptr<GeoZoneCredentialStore> store;
    return store;
}

} // namespace

GeoZoneCredentials::GeoZoneCredentials(const QString &settingsGroup)
    : _settings(settingsGroup)
{
}

QString GeoZoneCredentials::_id(const QString &sourceKey)
{
    return QString::fromLatin1(QCryptographicHash::hash(sourceKey.toUtf8(), QCryptographicHash::Sha256).toHex());
}

GeoZoneCredentialStore *GeoZoneCredentials::_store()
{
    return installedStore().get();
}

QString GeoZoneCredentials::secret(const QString &sourceKey) const
{
    // A secret is only in the settings without a store, when the store failed to keep it, or until it is moved
    // into the store, so it is the latest one
    const QString id = _id(sourceKey);
    if (const QString secret = _settings.secret(id); !secret.isEmpty() || !_store()) {
        return secret;
    }
    return _store()->secret(id);
}

void GeoZoneCredentials::setSecret(const QString &sourceKey, const QString &secret)
{
    const QString id = _id(sourceKey);
    if (GeoZoneCredentialStore *const store = _store()) {
        if (store->setSecret(id, secret)) {
            // No copy is left in the settings
            (void) _settings.setSecret(id, QString());
            return;
        }
        if (!secret.isEmpty()) {
            qCWarning(GeoZoneSourceLog) << "The secret of a geozone service could not be kept in" << store->name() << "but in the settings";
        }
    }
    (void) _settings.setSecret(id, secret);
}

int GeoZoneCredentials::setStore(std::unique_ptr<GeoZoneCredentialStore> store)
{
    installedStore() = std::move(store);
    GeoZoneCredentialStore *const installed = _store();
    if (!installed) {
        return 0;
    }
    GeoZoneSettingsCredentialStore settings;
    int moved = 0;
    for (const QString &id : settings.ids()) {
        const QString secret = settings.secret(id);
        if (!secret.isEmpty() && installed->setSecret(id, secret) && settings.setSecret(id, QString())) {
            moved++;
        }
    }
    if (moved > 0) {
        qCDebug(GeoZoneSourceLog) << "Moved" << moved << "geozone service secrets from the settings to" << installed->name();
    }
    return moved;
}

QString GeoZoneCredentials::storeName()
{
    return _store() ? _store()->name() : GeoZoneSettingsCredentialStore().name();
}

bool GeoZoneCredentials::storeIsSecure()
{
    return _store() && _store()->isSecure();
}
