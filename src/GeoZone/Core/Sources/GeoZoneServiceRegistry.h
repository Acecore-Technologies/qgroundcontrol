/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QString>

#include "GeoZoneCredentials.h"
#include "GeoZoneService.h"

/// The services of the feeds requested from one, by source key, with their secrets (GeoZoneCredentials) and the OAuth2
/// tokens obtained for them during the session. A secret is kept for the authentication target of its service, see
/// GeoZoneService::sameAuthTarget: setting a service which sends it elsewhere forgets it.
class GeoZoneServiceRegistry
{
public:
    bool contains(const QString &key) const { return _services.contains(key); }
    /// @return The service of the source, empty for plain feeds and files
    GeoZoneService service(const QString &key) const { return _services.value(key); }
    /// Sets the service of a source, removes it when empty. The secret and the token are forgotten unless the
    /// source had a service with the same authentication target.
    void setService(const QString &key, const GeoZoneService &service);
    /// Restores a service kept by a previous session, keeping its secret
    void restoreService(const QString &key, const GeoZoneService &service);
    /// Forgets the service of a source with its secret and token
    void forget(const QString &key);

    QString secret(const QString &key) const { return _credentials.secret(key); }
    /// Replaces the secret of a source, forgetting its token
    void setSecret(const QString &key, const QString &secret);
    /// @return true when the service of the source needs a secret which isn't known
    bool credentialsMissing(const QString &key) const;
    GeoZoneCredentials &credentials() { return _credentials; }

    /// @return The OAuth2 token of the source, empty when it has none valid at the time
    QString token(const QString &key, const QDateTime &time) const;
    void setToken(const QString &key, const QString &token, const QDateTime &expiresAt);
    void forgetToken(const QString &key) { (void) _tokens.remove(key); }

private:
    struct Token {
        QString value;
        QDateTime expiresAt;
    };

    GeoZoneCredentials _credentials;
    QHash<QString, GeoZoneService> _services;   ///< By source key
    QHash<QString, Token> _tokens;              ///< Kept for the session
};
