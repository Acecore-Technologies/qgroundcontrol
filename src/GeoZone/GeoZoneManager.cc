/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneManager.h"
#include "GeoZoneDisplay.h"
#include "GeoZoneEnvironment.h"
#include "GeoZonePlanning.h"
#include "GeoZoneInfo.h"
#include "GeoZoneSettings.h"
#include "GeoZoneAuthorisations.h"
#include "GeoZoneShape.h"
#include "GeoZoneSource.h"
#include "GeoZoneSourceRepository.h"
#include "MultiVehicleManager.h"
#include "QGCLoggingCategory.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"

#include <QtCore/qapplicationstatic.h>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QLocale>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUrl>

#include <algorithm>

QGC_LOGGING_CATEGORY(GeoZoneManagerLog, "qgc.geozone.geozonemanager")

Q_APPLICATION_STATIC(GeoZoneManager, _geoZoneManager);

namespace {

QString defaultStorageDir()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("GeoZones"));
}

QString toLocalFileName(const QString &fileName)
{
    return fileName.startsWith(QStringLiteral("file:")) ? QUrl(fileName).toLocalFile() : fileName;
}

/// @return The publishers of the trustedPackPublishers setting, a JSON array of objects with name and publicKey
QList<GeoZonePackSignature::Publisher> parsePublishers(const QString &json)
{
    QList<GeoZonePackSignature::Publisher> publishers;
    for (const QJsonValue &value : QJsonDocument::fromJson(json.toUtf8()).array()) {
        const QJsonObject object = value.toObject();
        const QByteArray publicKey = GeoZonePackSignature::parsePublicKey(object.value(QStringLiteral("publicKey")).toString());
        if (!publicKey.isEmpty()) {
            publishers.append(GeoZonePackSignature::Publisher{ object.value(QStringLiteral("name")).toString(), publicKey });
        }
    }
    return publishers;
}

QString publishersJson(const QList<GeoZonePackSignature::Publisher> &publishers)
{
    QJsonArray array;
    for (const GeoZonePackSignature::Publisher &publisher : publishers) {
        array.append(QJsonObject{ { QStringLiteral("name"), publisher.name }, { QStringLiteral("publicKey"), QString::fromLatin1(publisher.publicKey.toBase64()) } });
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

} // namespace

GeoZoneManager::GeoZoneManager(const QString &storageDir, QObject *parent)
    : QObject(parent)
    , _sources(new QmlObjectListModel(this))
    , _repository(new GeoZoneSourceRepository(storageDir.isEmpty() ? defaultStorageDir() : storageDir, this))
    , _display(new GeoZoneDisplay(this))
    , _planning(new GeoZonePlanning(QDir(_repository->storageDir()).filePath(GeoZoneAuthorisations::fileName), this))
    , _activeTimer(new QTimer(this))
{
    (void) connect(_repository, &GeoZoneSourceRepository::sourceAppended, _sources, [this](GeoZoneSource *source) { _sources->append(source); });
    (void) connect(_repository, &GeoZoneSourceRepository::sourceRemoved, _sources, [this](int index) { (void) _sources->removeAt(index); });

    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    // The age at which data is outdated may have changed
    (void) connect(settings->staleDays(), &Fact::rawValueChanged, _repository, &GeoZoneSourceRepository::updateValidity);
    (void) connect(settings->liveStaleHours(), &Fact::rawValueChanged, _repository, &GeoZoneSourceRepository::updateValidity);
    // Limits published as flight levels are converted with the QNH, so checks and texts using them are updated
    (void) connect(settings->qnhHPa(), &Fact::rawValueChanged, this, [this]() { emit zonesChanged(nullptr); });
    _repository->setAreaMargin(settings->areaOfInterestMargin()->rawValue().toDouble());
    _repository->setFollowArea(settings->followAreaOfInterest()->rawValue().toBool());
    (void) connect(settings->areaOfInterestMargin(), &Fact::rawValueChanged, _repository, [this](const QVariant &value) { _repository->setAreaMargin(value.toDouble()); });
    (void) connect(settings->followAreaOfInterest(), &Fact::rawValueChanged, _repository, [this](const QVariant &value) { _repository->setFollowArea(value.toBool()); });
    _updatePackSigning();
    _updateTrustedPublishers();
    (void) connect(settings->packSigningKeyFile(), &Fact::rawValueChanged, this, &GeoZoneManager::_updatePackSigning);
    (void) connect(settings->packPublisherName(), &Fact::rawValueChanged, this, &GeoZoneManager::_updatePackSigning);
    (void) connect(settings->trustedPackPublishers(), &Fact::rawValueChanged, this, &GeoZoneManager::_updateTrustedPublishers);

    _activeTimer->setInterval(_activeUpdateIntervalMSecs);
    (void) connect(_activeTimer, &QTimer::timeout, this, &GeoZoneManager::_updateActive);

    (void) connect(_repository, &GeoZoneSourceRepository::sourceAboutToChange, this, &GeoZoneManager::zonesAboutToChange);
    (void) connect(_repository, &GeoZoneSourceRepository::sourceChanged, this, &GeoZoneManager::_endZonesChange);
    (void) connect(_repository, &GeoZoneSourceRepository::userLoaded, _display, [this]() { _display->setVisible(true); });
    (void) connect(_repository, &GeoZoneSourceRepository::sourceAdded, this, &GeoZoneManager::sourceAdded);
    (void) connect(_repository, &GeoZoneSourceRepository::loadFailed, this, &GeoZoneManager::loadFailed);
    (void) connect(_repository, &GeoZoneSourceRepository::storageFailed, this, &GeoZoneManager::storageFailed);
    (void) connect(_planning->authorisations(), &GeoZoneAuthorisations::saveFailed, this, &GeoZoneManager::storageFailed);
    (void) connect(_repository, &GeoZoneSourceRepository::outdatedDataFound, this, &GeoZoneManager::outdatedDataFound);
    (void) connect(_repository, &GeoZoneSourceRepository::busyChanged, this, &GeoZoneManager::busyChanged);
    (void) connect(_repository, &GeoZoneSourceRepository::progressChanged, this, &GeoZoneManager::progressChanged);
    (void) connect(_repository, &GeoZoneSourceRepository::validityWarningChanged, this, &GeoZoneManager::validityWarningChanged);
}

GeoZoneManager::~GeoZoneManager()
{
}

GeoZoneManager *GeoZoneManager::instance()
{
    return _geoZoneManager();
}

void GeoZoneManager::init()
{
    _repository->init();

    // The services taking the area of interest download the zones around the active vehicle
    MultiVehicleManager *const vehicles = MultiVehicleManager::instance();
    (void) connect(vehicles, &MultiVehicleManager::activeVehicleChanged, this, &GeoZoneManager::_activeVehicleChanged);
    _activeVehicleChanged(vehicles->activeVehicle());
}

void GeoZoneManager::_activeVehicleChanged(Vehicle *vehicle)
{
    (void) disconnect(_vehicleConnection);
    if (!vehicle) {
        _repository->setVehiclePosition(QGeoCoordinate());
        return;
    }
    _vehicleConnection = connect(vehicle, &Vehicle::coordinateChanged, _repository, &GeoZoneSourceRepository::setVehiclePosition);
    _repository->setVehiclePosition(vehicle->coordinate());
}

QStringList GeoZoneManager::fileDialogFilters() const
{
    // The format is recognized by the content, zip packages are unpacked
    return { tr("Geozone Files (*.json *.geojson *.xml *.zip)"), tr("All Files (*)") };
}

bool GeoZoneManager::exportProvenance(const QString &fileName)
{
    const QString localFileName = fileName.startsWith(QStringLiteral("file:")) ? QUrl(fileName).toLocalFile() : fileName;
    QSaveFile file(localFileName);
    const QByteArray json = QJsonDocument(provenanceReport()).toJson();
    if (!file.open(QIODevice::WriteOnly) || (file.write(json) != json.size()) || !file.commit()) {
        emit fileOperationFinished(false, tr("The geozone source provenance could not be saved to %1: %2").arg(localFileName, file.errorString()));
        return false;
    }
    emit fileOperationFinished(true, tr("The geozone source provenance was saved to %1.").arg(localFileName));
    return true;
}

bool GeoZoneManager::exportPack(const QString &fileName, const QStringList &keys)
{
    const QString localFileName = fileName.startsWith(QStringLiteral("file:")) ? QUrl(fileName).toLocalFile() : fileName;
    QStringList sourceKeys = keys;
    if (sourceKeys.isEmpty()) {
        for (const GeoZoneSource *const source : _repository->sources()) {
            if (source->enabled()) {
                sourceKeys.append(source->key());
            }
        }
    }
    QString errorString;
    if (!_repository->exportPack(sourceKeys, localFileName, errorString)) {
        emit fileOperationFinished(false, tr("The geozone pack could not be saved to %1: %2").arg(localFileName, errorString));
        return false;
    }
    const QString keyId = packKeyId();
    emit fileOperationFinished(true, keyId.isEmpty() ? tr("The geozone pack was saved to %1, unsigned.").arg(localFileName)
                                                     : tr("The geozone pack was saved to %1, signed with the key %2.").arg(localFileName, keyId));
    return true;
}

bool GeoZoneManager::importPack(const QString &fileName)
{
    const QString localFileName = fileName.startsWith(QStringLiteral("file:")) ? QUrl(fileName).toLocalFile() : fileName;
    QString errorString;
    QString signatureText;
    const int count = _repository->importPack(localFileName, errorString, &signatureText);
    if (count < 0) {
        emit fileOperationFinished(false, errorString);
        return false;
    }
    QString message = tr("%n geozone source(s) imported from %1.", nullptr, count).arg(QFileInfo(localFileName).fileName()) + QLatin1Char(' ') + signatureText;
    if (!errorString.isEmpty()) {
        message += QStringLiteral("\n") + tr("Left out:") + QStringLiteral("\n") + errorString;
    }
    emit fileOperationFinished(errorString.isEmpty(), message);
    return true;
}

void GeoZoneManager::_updatePackSigning()
{
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    _repository->setPackSigner(toLocalFileName(settings->packSigningKeyFile()->rawValue().toString().trimmed()), settings->packPublisherName()->rawValue().toString());
    emit packSigningChanged();
}

void GeoZoneManager::_updateTrustedPublishers()
{
    _repository->setTrustedPublishers(parsePublishers(GeoZoneSettings::instance()->trustedPackPublishers()->rawValue().toString()));
    emit trustedPublishersChanged();
}

QByteArray GeoZoneManager::_packPublicKey() const
{
    QFile keyFile(toLocalFileName(GeoZoneSettings::instance()->packSigningKeyFile()->rawValue().toString().trimmed()));
    if (keyFile.fileName().isEmpty() || !keyFile.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    QString errorString;
    return GeoZonePackSignature::publicKey(keyFile.readAll(), errorString);
}

QString GeoZoneManager::packPublicKey() const
{
    return QString::fromLatin1(_packPublicKey().toBase64());
}

QString GeoZoneManager::packKeyId() const
{
    const QByteArray publicKey = _packPublicKey();
    return publicKey.isEmpty() ? QString() : GeoZonePackSignature::keyId(publicKey);
}

QVariantList GeoZoneManager::trustedPublishers() const
{
    QVariantList publishers;
    for (const GeoZonePackSignature::Publisher &publisher : _repository->trustedPublishers()) {
        publishers.append(QVariantMap{
            { QStringLiteral("name"), publisher.name },
            { QStringLiteral("publicKey"), QString::fromLatin1(publisher.publicKey.toBase64()) },
            { QStringLiteral("keyId"), GeoZonePackSignature::keyId(publisher.publicKey) },
        });
    }
    return publishers;
}

bool GeoZoneManager::trustPublisher(const QString &name, const QString &publicKey)
{
    const QByteArray key = GeoZonePackSignature::parsePublicKey(publicKey);
    if (key.isEmpty()) {
        emit fileOperationFinished(false, tr("The public key is not an Ed25519 public key in base64 or PEM."));
        return false;
    }
    QList<GeoZonePackSignature::Publisher> publishers = _repository->trustedPublishers();
    (void) publishers.removeIf([&key](const GeoZonePackSignature::Publisher &publisher) { return publisher.publicKey == key; });
    publishers.append(GeoZonePackSignature::Publisher{ name.trimmed().isEmpty() ? GeoZonePackSignature::keyId(key) : name.trimmed(), key });
    GeoZoneSettings::instance()->trustedPackPublishers()->setRawValue(publishersJson(publishers));
    return true;
}

void GeoZoneManager::distrustPublisher(const QString &publicKey)
{
    const QByteArray key = GeoZonePackSignature::parsePublicKey(publicKey);
    QList<GeoZonePackSignature::Publisher> publishers = _repository->trustedPublishers();
    if (publishers.removeIf([&key](const GeoZonePackSignature::Publisher &publisher) { return publisher.publicKey == key; }) > 0) {
        GeoZoneSettings::instance()->trustedPackPublishers()->setRawValue(publishersJson(publishers));
    }
}

bool GeoZoneManager::trustSourcePublisher(const QString &sourceKey)
{
    const GeoZoneSource *const source = this->source(sourceKey);
    if (!source || source->provenance().packPublicKey.isEmpty()) {
        return false;
    }
    return trustPublisher(source->provenance().packSigner, QString::fromLatin1(source->provenance().packPublicKey.toBase64()));
}

bool GeoZoneManager::createSigningKey(const QString &fileName)
{
    const QString localName = toLocalFileName(fileName);
    const QByteArray privateKey = GeoZonePackSignature::generatePrivateKey();
    QSaveFile file(localName);
    if (privateKey.isEmpty() || !file.open(QIODevice::WriteOnly) || (file.write(privateKey) != privateKey.size()) || !file.commit()) {
        emit fileOperationFinished(false, tr("The pack signing key could not be saved to %1: %2").arg(localName, privateKey.isEmpty() ? tr("signing isn't available") : file.errorString()));
        return false;
    }
    (void) QFile::setPermissions(localName, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    GeoZoneSettings::instance()->packSigningKeyFile()->setRawValue(localName);
    emit fileOperationFinished(true, tr("A new pack signing key was saved to %1. Keep it private; the public key shown below lets others trust your packs.").arg(localName));
    return true;
}

bool GeoZoneManager::addArea(const QString &provider, const QGeoRectangle &area)
{
    const QString url = areaFeedUrl(provider, area);
    if (url.isEmpty()) {
        emit loadFailed(provider, tr("Geozones can be downloaded for an area up to %1 degrees wide and high. Zoom in on the map.").arg(GeoZoneProviders::maxAreaDegrees));
        return false;
    }
    addUrl(url);
    return true;
}

int GeoZoneManager::disabledSourceCount() const
{
    const QList<const GeoZoneSource*> sources = _repository->sourceList();
    return static_cast<int>(std::count_if(sources.cbegin(), sources.cend(), [](const GeoZoneSource *source) { return !source->enabled(); }));
}

void GeoZoneManager::_endZonesChange(GeoZoneSource *source)
{
    const QGeoRectangle previousBoundingBox = _catalog.boundingBox();
    QList<const GeoZoneSource*> sources = _repository->sourceList();
    (void) sources.removeIf([](const GeoZoneSource *enabledSource) { return !enabledSource->enabled(); });
    _catalog.rebuild(sources);

    const QList<GeoZone*> &zones = _catalog.zones();
    if (zones.isEmpty()) {
        _activeTimer->stop();
    } else if (!_activeTimer->isActive()) {
        _activeTimer->start();
    }

    emit zonesChanged(source);
    emit zoneCountChanged();
    if (_catalog.boundingBox() != previousBoundingBox) {
        emit boundingBoxChanged();
    }
}

void GeoZoneManager::_updateActive()
{
    const QDateTime now = GeoZoneEnvironment::currentDateTimeUtc();
    bool changed = false;
    for (GeoZone *zone : _catalog.zones()) {
        changed = zone->updateActive(now) || changed;
    }
    _repository->updateValidity();
    if (changed) {
        emit activeZonesChanged();
    }
}

QVariantList GeoZoneManager::labelZones(const QGeoRectangle &region) const
{
    return GeoZoneInfo::toVariantList(_catalog.labelZones(region, maxLabelCount));
}

QVariantList GeoZoneManager::zonesAt(const QGeoCoordinate &coordinate) const
{
    QList<const GeoZone*> zones = _catalog.zonesAt(coordinate);
    const int hidden = _display->hiddenRestrictions();
    (void) zones.removeIf([hidden](const GeoZone *zone) { return (hidden & (1 << zone->restriction())) != 0; });
    return GeoZoneInfo::toVariantList(zones);
}

QVariantList GeoZoneManager::searchZones(const QString &text, int maxResults) const
{
    return GeoZoneInfo::toVariantList(_catalog.search(text, maxResults));
}

void GeoZoneManager::showZone(const QVariant &zone)
{
    if (GeoZoneInfo::fromVariant(zone)) {
        emit showZoneRequested(zone);
    }
}
