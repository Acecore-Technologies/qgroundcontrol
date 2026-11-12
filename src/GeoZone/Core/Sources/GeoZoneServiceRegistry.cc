/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneServiceRegistry.h"

void GeoZoneServiceRegistry::setService(const QString &key, const GeoZoneService &service)
{
    // A secret only goes where it was entered for
    if (const auto previous = _services.constFind(key); (previous == _services.cend()) || !previous->sameAuthTarget(service)) {
        (void) _credentials.remove(key);
        forgetToken(key);
    }
    if (service.isEmpty()) {
        (void) _services.remove(key);
    } else {
        _services.insert(key, service);
    }
}

void GeoZoneServiceRegistry::restoreService(const QString &key, const GeoZoneService &service)
{
    if (!service.isEmpty()) {
        _services.insert(key, service);
    }
}

void GeoZoneServiceRegistry::forget(const QString &key)
{
    if (_services.remove(key) > 0) {
        (void) _credentials.remove(key);
    }
    forgetToken(key);
}

bool GeoZoneServiceRegistry::setSecret(const QString &key, const QString &secret)
{
    if (const auto it = _services.constFind(key); (it == _services.cend()) || !it->needsSecret()) {
        return false;
    }
    // When it can't be kept, the source shows that it is missing
    (void) _credentials.setSecret(key, secret);
    forgetToken(key);
    return true;
}

bool GeoZoneServiceRegistry::credentialsMissing(const QString &key) const
{
    const auto it = _services.constFind(key);
    return (it != _services.cend()) && it->needsSecret() && _credentials.secret(key).isEmpty();
}

QString GeoZoneServiceRegistry::token(const QString &key, const QDateTime &time) const
{
    const auto it = _tokens.constFind(key);
    return ((it != _tokens.cend()) && (it->expiresAt > time)) ? it->value : QString();
}

void GeoZoneServiceRegistry::setToken(const QString &key, const QString &token, const QDateTime &expiresAt)
{
    _tokens.insert(key, Token{ token, expiresAt });
}
