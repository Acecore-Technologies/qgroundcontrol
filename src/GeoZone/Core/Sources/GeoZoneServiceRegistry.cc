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
        _credentials.remove(key);
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
        _credentials.remove(key);
    }
    forgetToken(key);
}

void GeoZoneServiceRegistry::setSecret(const QString &key, const QString &secret)
{
    _credentials.setSecret(key, secret);
    forgetToken(key);
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
