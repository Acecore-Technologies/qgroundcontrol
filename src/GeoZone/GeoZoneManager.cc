/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneManager.h"
#include "GeoZoneAlertTracker.h"
#include "GeoZoneDisplay.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneOperatorZones.h"
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
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QtMath>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkInformation>

#include <algorithm>
#include <cmath>
#include <memory>

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

/// @return The publishers of the trustedPackPublishers setting, a JSON array of objects with name and publicKey:
///         those which can be read
/// @param unreadable Set to the number of entries which aren't a publisher with a public key, -1 when the text
///                   isn't a JSON array at all. The setting must not be written from the publishers returned then,
///                   which would drop what couldn't be read.
QList<GeoZonePackSignature::Publisher> parsePublishers(const QString &json, int &unreadable)
{
    QList<GeoZonePackSignature::Publisher> publishers;
    unreadable = 0;
    if (json.trimmed().isEmpty()) {
        return publishers;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &parseError);
    if ((parseError.error != QJsonParseError::NoError) || !document.isArray()) {
        unreadable = -1;
        return publishers;
    }
    for (const QJsonValue &value : document.array()) {
        const QJsonObject object = value.toObject();
        const QByteArray publicKey = GeoZonePackSignature::parsePublicKey(object.value(QStringLiteral("publicKey")).toString());
        if (publicKey.isEmpty()) {
            unreadable++;
        } else {
            publishers.append(GeoZonePackSignature::Publisher{ object.value(QStringLiteral("name")).toString(), publicKey });
        }
    }
    return publishers;
}

/// Writes a file only its owner can read, like a private key: the permissions are set before the content is written
/// and the file gets its name, so the content is never readable by others
/// @return false with errorString set when the file can't be written or its permissions can't be set
bool writePrivateFile(const QString &fileName, const QByteArray &content, QString &errorString)
{
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly)) {
        errorString = file.errorString();
        return false;
    }
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        errorString = GeoZoneManager::tr("the file can't be made readable by its owner only (%1)").arg(file.errorString());
        file.cancelWriting();
        return false;
    }
    if ((file.write(content) != content.size()) || !file.commit()) {
        errorString = file.errorString();
        return false;
    }
    return true;
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
    , _operatorZones(new GeoZoneOperatorZones(_repository, this))
    , _activeTimer(new QTimer(this))
    , _mapViewTimer(new QTimer(this))
{
    (void) connect(_repository, &GeoZoneSourceRepository::sourceAppended, _sources, [this](GeoZoneSource *source) { _sources->append(source); });
    (void) connect(_repository, &GeoZoneSourceRepository::sourceRemoved, _sources, [this](int index) { (void) _sources->removeAt(index); });

    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    // The age at which data is outdated, or whether it is invalid then, may have changed
    (void) connect(settings->staleDays(), &Fact::rawValueChanged, _repository, &GeoZoneSourceRepository::updateValidity);
    (void) connect(settings->outdatedInvalid(), &Fact::rawValueChanged, _repository, &GeoZoneSourceRepository::updateValidity);
    // Limits published as flight levels are converted with the QNH, so checks and texts using them are updated
    (void) connect(settings->qnhHPa(), &Fact::rawValueChanged, this, [this]() { emit zonesChanged(nullptr); });
    _repository->setFollowArea(settings->followAreaOfInterest()->rawValue().toBool());
    (void) connect(settings->followAreaOfInterest(), &Fact::rawValueChanged, _repository, [this](const QVariant &value) { _repository->setFollowArea(value.toBool()); });
    _repository->setFollowView(settings->followMapView()->rawValue().toBool());
    (void) connect(settings->followMapView(), &Fact::rawValueChanged, _repository, [this](const QVariant &value) { _repository->setFollowView(value.toBool()); });
    _repository->feedCatalogue()->setUrl(settings->feedCatalogueUrl()->rawValue().toString());
    (void) connect(settings->feedCatalogueUrl(), &Fact::rawValueChanged, _repository, [this](const QVariant &value) { _repository->feedCatalogue()->setUrl(value.toString()); });
    (void) connect(_repository->feedCatalogue(), &GeoZoneFeedCatalogueLoader::changed, this, &GeoZoneManager::feedCatalogueChanged);
    _mapViewTimer->setSingleShot(true);
    _mapViewTimer->setInterval(mapViewDelayMSecs);
    (void) connect(_mapViewTimer, &QTimer::timeout, this, [this]() { _repository->setViewArea(_mapView); });
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
    (void) connect(_repository, &GeoZoneSourceRepository::sourceAdded, this, &GeoZoneManager::_noteSharedZones);
    (void) connect(_repository, &GeoZoneSourceRepository::sourceAdded, this, &GeoZoneManager::sourceAdded);
    (void) connect(_repository, &GeoZoneSourceRepository::loadFailed, this, &GeoZoneManager::loadFailed);
    (void) connect(_repository, &GeoZoneSourceRepository::filesAdded, this, &GeoZoneManager::_filesAdded);
    (void) connect(_repository, &GeoZoneSourceRepository::storageFailed, this, &GeoZoneManager::storageFailed);
    (void) connect(_planning->authorisations(), &GeoZoneAuthorisations::saveFailed, this, &GeoZoneManager::storageFailed);
    (void) connect(_planning->authorisations(), &GeoZoneAuthorisations::loadFailed, this, &GeoZoneManager::storageFailed);
    (void) connect(_repository, &GeoZoneSourceRepository::outdatedDataFound, this, &GeoZoneManager::outdatedDataFound);
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
    // The zones the operator drew are a source like the others, also when the index of the sources lost it
    _operatorZones->restoreSource();

    // The services taking the area of interest download the zones around the active vehicle
    MultiVehicleManager *const vehicles = MultiVehicleManager::instance();
    (void) connect(vehicles, &MultiVehicleManager::activeVehicleChanged, this, &GeoZoneManager::_activeVehicleChanged);
    _activeVehicleChanged(vehicles->activeVehicle());
}

void GeoZoneManager::watchNetwork()
{
    if (!QNetworkInformation::instance() && !QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Reachability)) {
        qCDebug(GeoZoneManagerLog) << "No network information backend, feeds are refreshed whatever the network";
        return;
    }
    QNetworkInformation *const information = QNetworkInformation::instance();
    if (!information || !information->supports(QNetworkInformation::Feature::Reachability)) {
        return;
    }
    // A local network only is like an unknown one: the refreshes are tried
    const auto network = [](QNetworkInformation::Reachability reachability) {
        switch (reachability) {
        case QNetworkInformation::Reachability::Online:
            return GeoZoneRefreshScheduler::Network::Online;
        case QNetworkInformation::Reachability::Disconnected:
            return GeoZoneRefreshScheduler::Network::Offline;
        default:
            return GeoZoneRefreshScheduler::Network::Unknown;
        }
    };
    _repository->setNetwork(network(information->reachability()));
    (void) connect(information, &QNetworkInformation::reachabilityChanged, _repository, [this, network](QNetworkInformation::Reachability reachability) {
        _repository->setNetwork(network(reachability));
    });
}

void GeoZoneManager::_activeVehicleChanged(Vehicle *vehicle)
{
    (void) disconnect(_vehicleConnection);
    if (!vehicle) {
        _repository->setVehiclePosition(QGeoCoordinate());
        return;
    }
    // The zones ahead of the vehicle are downloaded before it reaches them, see GeoZoneAreaOfInterest: by its velocity
    // from its positions, and by the ground speed it reports when that is higher or the positions are too few
    struct Motion {
        GeoZoneVelocityEstimator estimator;
        QElapsedTimer clock;
    };
    const std::shared_ptr<Motion> motion = std::make_shared<Motion>();
    motion->clock.start();
    const auto moved = [this, vehicle, motion](const QGeoCoordinate &coordinate) {
        double speed = 0;
        double course = qQNaN();
        if (coordinate.isValid()) {
            const GeoZoneAlertTracker::Velocity velocity = motion->estimator.add(motion->clock.elapsed(), GeoZone::Position{ coordinate });
            if (velocity.valid && (velocity.groundSpeed() >= GeoZoneAlertTracker::minPredictionSpeed)) {
                speed = velocity.groundSpeed();
                course = qRadiansToDegrees(std::atan2(velocity.east, velocity.north));
            }
        }
        if (const double reported = vehicle->groundSpeed()->rawValue().toDouble(); std::isfinite(reported)) {
            speed = (std::max)(speed, reported);
        }
        _repository->setVehiclePosition(coordinate, speed, course);
    };
    _vehicleConnection = connect(vehicle, &Vehicle::coordinateChanged, _repository, moved);
    moved(vehicle->coordinate());
}

QStringList GeoZoneManager::fileDialogFilters() const
{
    // The format is recognized by the content, zip packages are unpacked
    return { tr("Geozone Files (*.json *.geojson *.xml *.zip)"), tr("All Files (*)") };
}

bool GeoZoneManager::exportPack(const QString &fileName, const QStringList &keys)
{
    const QString localFileName = toLocalFileName(fileName);
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
    return importPacks({ fileName });
}

bool GeoZoneManager::importPacks(const QStringList &fileNames)
{
    bool readable = true;
    bool complete = true;
    QStringList messages;
    for (const QString &fileName : fileNames) {
        QString message;
        bool packComplete = false;
        readable = _importPack(fileName, message, packComplete) && readable;
        complete = complete && packComplete;
        messages.append(message);
    }
    if (!messages.isEmpty()) {
        emit fileOperationFinished(complete, messages.join(QStringLiteral("\n\n")));
    }
    return readable;
}

bool GeoZoneManager::_importPack(const QString &fileName, QString &message, bool &complete)
{
    const QString localFileName = toLocalFileName(fileName);
    QString errorString;
    QString signatureText;
    const int count = _repository->importPack(localFileName, errorString, &signatureText);
    complete = (count >= 0) && errorString.isEmpty();
    if (count < 0) {
        message = errorString;
        return false;
    }
    message = tr("%n geozone source(s) imported from %1.", nullptr, count).arg(QFileInfo(localFileName).fileName()) + QLatin1Char(' ') + signatureText;
    if (!errorString.isEmpty()) {
        message += QStringLiteral("\n") + tr("Left out:") + QStringLiteral("\n") + errorString;
    }
    return true;
}

void GeoZoneManager::_filesAdded(int addedCount, const QStringList &failures)
{
    const QString added = tr("%n geozone file(s) added", nullptr, addedCount);
    if (failures.isEmpty()) {
        emit fileOperationFinished(true, tr("%1.", "Number of geozone files added").arg(added));
        return;
    }
    const QString failed = tr("%n failed", "Number of geozone files which failed to load", static_cast<int>(failures.count()));
    emit fileOperationFinished(false, tr("%1, %2:", "Number of geozone files added and failed, followed by the reasons").arg(added, failed) +
                                          QStringLiteral("\n") + failures.join(QStringLiteral("\n")));
}

void GeoZoneManager::_updatePackSigning()
{
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    _repository->setPackSigner(toLocalFileName(settings->packSigningKeyFile()->rawValue().toString().trimmed()), settings->packPublisherName()->rawValue().toString());
    emit packSigningChanged();
}

void GeoZoneManager::_updateTrustedPublishers()
{
    // Publishers which can't be read aren't trusted
    int unreadable = 0;
    const QList<GeoZonePackSignature::Publisher> publishers = parsePublishers(GeoZoneSettings::instance()->trustedPackPublishers()->rawValue().toString(), unreadable);
    if (unreadable == 0) {
        _trustedPublishersWarning.clear();
    } else {
        _trustedPublishersWarning = (unreadable < 0) ? tr("The trusted publishers kept in the settings can't be read. None is trusted, and the setting is left as it is.")
                                                     : tr("%n trusted publisher(s) kept in the settings can't be read and aren't trusted. The setting is left as it is.", nullptr, unreadable);
        qCWarning(GeoZoneManagerLog) << "Trusted pack publishers setting can't be read, unreadable entries:" << unreadable;
    }
    _repository->setTrustedPublishers(publishers);
    emit trustedPublishersChanged();
}

bool GeoZoneManager::_trustedPublishersWritable()
{
    if (_trustedPublishersWarning.isEmpty()) {
        return true;
    }
    emit fileOperationFinished(false, tr("%1 Discard the unreadable publishers in the pack signatures of the Geozones settings before changing the trusted publishers.")
                                          .arg(_trustedPublishersWarning));
    return false;
}

void GeoZoneManager::discardUnreadablePublishers()
{
    if (!_trustedPublishersWarning.isEmpty()) {
        GeoZoneSettings::instance()->trustedPackPublishers()->setRawValue(publishersJson(_repository->trustedPublishers()));
    }
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
    // The setting is never written from publishers read in part, which would drop the others
    if (!_trustedPublishersWritable()) {
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
    if (!GeoZonePackSignature::find(publishers, key) || !_trustedPublishersWritable()) {
        return;
    }
    if (publishers.removeIf([&key](const GeoZonePackSignature::Publisher &publisher) { return publisher.publicKey == key; }) > 0) {
        GeoZoneSettings::instance()->trustedPackPublishers()->setRawValue(publishersJson(publishers));
    }
}

bool GeoZoneManager::trustSourcePublisher(const QString &sourceKey)
{
    // Only keys whose signature was checked
    const GeoZoneSource *const source = _repository->source(sourceKey);
    if (!source || !source->untrustedPack() || source->provenance().packPublicKey.isEmpty()) {
        return false;
    }
    return trustPublisher(source->provenance().packSigner, QString::fromLatin1(source->provenance().packPublicKey.toBase64()));
}

bool GeoZoneManager::createSigningKey(const QString &fileName)
{
    const QString localName = toLocalFileName(fileName);
    const QByteArray privateKey = GeoZonePackSignature::generatePrivateKey();
    QString errorString = tr("signing isn't available");
    if (privateKey.isEmpty() || !writePrivateFile(localName, privateKey, errorString)) {
        qCWarning(GeoZoneManagerLog) << "Pack signing key not saved" << localName << errorString;
        emit fileOperationFinished(false, tr("The pack signing key could not be saved to %1: %2").arg(localName, errorString));
        return false;
    }
    GeoZoneSettings::instance()->packSigningKeyFile()->setRawValue(localName);
    // A new key in the file selected already leaves the setting as it is: the key shown is read again
    _updatePackSigning();
    emit fileOperationFinished(true, tr("A new pack signing key was saved to %1. Keep it private; the public key shown below lets others trust your packs.").arg(localName));
    return true;
}

bool GeoZoneManager::addArea(const QString &provider, const QGeoRectangle &area)
{
    const QString url = GeoZoneProviders::areaFeedUrl(provider, area);
    if (url.isEmpty()) {
        emit loadFailed(provider, tr("Geozones can be downloaded for an area up to %1 degrees wide and high. Zoom in on the map.").arg(GeoZoneProviders::maxAreaDegrees));
        return false;
    }
    _repository->addUrl(url);
    return true;
}

bool GeoZoneManager::areaDownloaded(const QString &provider, const QGeoRectangle &area) const
{
    QStringList feedUrls;
    for (const GeoZoneSource *const source : _repository->sourceList()) {
        if (source->isFeed()) {
            feedUrls.append(source->url());
        }
    }
    return GeoZoneProviders::areaDownloaded(provider, feedUrls, area);
}

void GeoZoneManager::setMapView(const QGeoRectangle &view)
{
    if (!view.isValid()) {
        return;
    }
    _mapView = view;
    _mapViewTimer->start();
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

    // The note is about a source which was removed
    if (!_sourceNoteKey.isEmpty() && !_repository->source(_sourceNoteKey)) {
        clearSourceNote();
    }

    emit zonesChanged(source);
    emit zoneCountChanged();
    if (_catalog.boundingBox() != previousBoundingBox) {
        emit boundingBoxChanged();
    }
}

void GeoZoneManager::_noteSharedZones(GeoZoneSource *source)
{
    const int zoneCount = source->zoneCount();
    QString otherKey;
    int sharedCount = 0;
    if (source->enabled() && (zoneCount > 0)) {
        const QHash<QString, int> counts = _catalog.sharedZoneCounts(source->key());
        for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
            if (it.value() > sharedCount) {
                otherKey = it.key();
                sharedCount = (std::min)(it.value(), zoneCount);
            }
        }
    }
    if (sharedCount <= (sharedSourceFraction * zoneCount)) {
        clearSourceNote();
        return;
    }
    const GeoZoneSource *const other = _repository->source(otherKey);
    _sourceNoteKey = source->key();
    _sourceNote = tr("%1 of the %n zone(s) of %2 are already loaded from %3. Zones both publish alike are shown and checked once.", "", zoneCount)
                      .arg(sharedCount).arg(source->name(), other ? other->name() : otherKey);
    emit sourceNoteChanged();
}

void GeoZoneManager::clearSourceNote()
{
    _sourceNoteKey.clear();
    if (!_sourceNote.isEmpty()) {
        _sourceNote.clear();
        emit sourceNoteChanged();
    }
}

bool GeoZoneManager::updateActive(const GeoZoneBox &area)
{
    const QDateTime now = GeoZoneEnvironment::currentDateTimeUtc();
    bool changed = false;
    for (GeoZone *zone : _catalog.zones()) {
        if (zone->box().intersects(area)) {
            changed = zone->updateActive(now) || changed;
        }
    }
    if (changed) {
        emit activeZonesChanged();
    }
    return changed;
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
