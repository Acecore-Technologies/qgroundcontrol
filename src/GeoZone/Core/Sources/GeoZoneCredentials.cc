/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneCredentials.h"
#include "GeoZoneHash.h"
#include "GeoZoneSource.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>

namespace {

constexpr const char *versionKey = "version";
constexpr const char *secretsKey = "secrets";
constexpr QFile::Permissions ownerOnly = QFile::ReadOwner | QFile::WriteOwner;

} // namespace

GeoZoneCredentials::GeoZoneCredentials(const QString &name)
    : _name(name)
{
    _migrate();
}

QString GeoZoneCredentials::_id(const QString &sourceKey)
{
    return QString::fromLatin1(GeoZoneHash::sha256(sourceKey.toUtf8()).toHex());
}

QString GeoZoneCredentials::filePath() const
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(_name + QStringLiteral(".json"));
}

QJsonObject GeoZoneCredentials::_read(bool &newer) const
{
    newer = false;
    QFile file(filePath());
    if (!file.exists()) {
        return QJsonObject();
    }
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(GeoZoneSourceLog) << "Unable to read the credentials of the geozone services" << file.errorString();
        return QJsonObject();
    }
    QJsonParseError parseError;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll(), &parseError).object();
    // The content, which holds secrets, is never logged
    if ((parseError.error != QJsonParseError::NoError) || !root.value(secretsKey).isObject()) {
        qCWarning(GeoZoneSourceLog) << "The credentials of the geozone services can't be read, enter them again";
        return QJsonObject();
    }
    if (root.value(versionKey).toInt() > fileVersion) {
        qCWarning(GeoZoneSourceLog) << "The credentials of the geozone services were saved by a newer version and are not used";
        newer = true;
        return QJsonObject();
    }
    return root.value(secretsKey).toObject();
}

bool GeoZoneCredentials::_write(const QJsonObject &secrets) const
{
    const QString path = filePath();
    if (secrets.isEmpty()) {
        return !QFile::exists(path) || QFile::remove(path);
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        qCWarning(GeoZoneSourceLog) << "Unable to keep the credentials of the geozone services, their folder can't be made";
        return false;
    }
    // Readable by its owner only before the first secret is written to it, and replaced as a whole
    QSaveFile file(path);
    const QByteArray bytes = QJsonDocument(QJsonObject{ { versionKey, fileVersion }, { secretsKey, secrets } }).toJson();
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(ownerOnly) || (file.write(bytes) != bytes.size()) || !file.commit()) {
        qCWarning(GeoZoneSourceLog) << "Unable to keep the credentials of the geozone services" << file.errorString();
        file.cancelWriting();
        return false;
    }
    return true;
}

void GeoZoneCredentials::_migrate() const
{
    QSettings settings;
    settings.beginGroup(_name);
    const QStringList ids = settings.childKeys();
    if (ids.isEmpty()) {
        return;
    }
    // Those of the file were entered since
    bool newer = false;
    QJsonObject secrets = _read(newer);
    for (const QString &id : ids) {
        const QString secret = settings.value(id).toString();
        if (!secret.isEmpty() && !secrets.contains(id)) {
            secrets.insert(id, secret);
        }
    }
    // Kept in the settings while the file can't be written, for the next start
    if (!newer && _write(secrets)) {
        settings.remove(QString());
        qCDebug(GeoZoneSourceLog) << "Moved the credentials of" << ids.count() << "geozone services out of the application settings";
    }
}

QString GeoZoneCredentials::secret(const QString &sourceKey) const
{
    bool newer = false;
    return _read(newer).value(_id(sourceKey)).toString();
}

bool GeoZoneCredentials::setSecret(const QString &sourceKey, const QString &secret)
{
    bool newer = false;
    QJsonObject secrets = _read(newer);
    if (newer) {
        return false;
    }
    const QString id = _id(sourceKey);
    if (secret.isEmpty()) {
        if (!secrets.contains(id)) {
            return true;
        }
        secrets.remove(id);
    } else {
        if (secrets.value(id).toString() == secret) {
            return true;
        }
        secrets.insert(id, secret);
    }
    return _write(secrets);
}
