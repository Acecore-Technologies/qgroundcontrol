/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFlightRecorder.h"
#include "AppSettings.h"
#include "Fact.h"
#include "GeoZoneAlertHistory.h"
#include "GeoZoneAuthorisations.h"
#include "GeoZoneBox.h"
#include "GeoZoneBriefing.h"
#include "GeoZoneBriefingExport.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneManager.h"
#include "GeoZoneMissionExport.h"
#include "GeoZonePathEvaluator.h"
#include "GeoZonePlanning.h"
#include "GeoZoneRemoteId.h"
#include "GeoZoneSettings.h"
#include "GeoZoneSourceRepository.h"
#include "GeoZoneSourceVersion.h"
#include "GeoZoneVehicleMonitor.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "Vehicle.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QTextStream>
#include <QtCore/qapplicationstatic.h>

#include <cmath>

Q_APPLICATION_STATIC(GeoZoneFlightRecorder, _geoZoneFlightRecorder);

namespace {

QJsonValue jsonTime(const QDateTime &dateTime)
{
    return dateTime.isValid() ? QJsonValue(dateTime.toUTC().toString(Qt::ISODateWithMs)) : QJsonValue(QJsonValue::Null);
}

QJsonValue jsonNumber(double value)
{
    return std::isfinite(value) ? QJsonValue(value) : QJsonValue(QJsonValue::Null);
}

QJsonValue jsonPosition(const GeoZone::Position &position)
{
    if (!position.coordinate.isValid()) {
        return QJsonValue(QJsonValue::Null);
    }
    return QJsonObject{
        { QStringLiteral("lat"), position.coordinate.latitude() },
        { QStringLiteral("lon"), position.coordinate.longitude() },
        { QStringLiteral("altitudeAMSL"), jsonNumber(position.altitudeAMSL) },
    };
}

QString firmwareText(Vehicle *vehicle)
{
    QString firmware = vehicle->firmwareTypeString();
    if (vehicle->firmwareMajorVersion() != Vehicle::versionNotSetValue) {
        firmware += QStringLiteral(" %1.%2.%3").arg(vehicle->firmwareMajorVersion()).arg(vehicle->firmwareMinorVersion()).arg(vehicle->firmwarePatchVersion());
    }
    return firmware;
}

} // namespace

GeoZoneFlightRecorder::GeoZoneFlightRecorder(QObject *parent)
    : GeoZoneFlightRecorder(GeoZoneManager::instance(), GeoZoneMonitor::instance(), parent)
{
    _sampleTimer.setInterval(sampleIntervalMSecs);
    (void) connect(&_sampleTimer, &QTimer::timeout, this, &GeoZoneFlightRecorder::_sampleVehicles);

    MultiVehicleManager *const multiVehicleManager = MultiVehicleManager::instance();
    (void) connect(multiVehicleManager, &MultiVehicleManager::vehicleAdded, this, &GeoZoneFlightRecorder::_addVehicle);
    (void) connect(multiVehicleManager, &MultiVehicleManager::vehicleRemoved, this, &GeoZoneFlightRecorder::_removeVehicle);
    for (int i = 0; i < multiVehicleManager->vehicles()->count(); i++) {
        _addVehicle(multiVehicleManager->vehicles()->value<Vehicle*>(i));
    }
}

GeoZoneFlightRecorder::GeoZoneFlightRecorder(GeoZoneManager *manager, GeoZoneMonitor *monitor, QObject *parent)
    : QObject(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
    , _monitor(monitor)
{
    if (_monitor) {
        (void) connect(_monitor, &GeoZoneMonitor::alertEvent, this, &GeoZoneFlightRecorder::_alertEvent);
    }
}

GeoZoneFlightRecorder *GeoZoneFlightRecorder::instance()
{
    return _geoZoneFlightRecorder();
}

void GeoZoneFlightRecorder::_addVehicle(Vehicle *vehicle)
{
    if (!vehicle) {
        return;
    }
    (void) connect(vehicle, &Vehicle::armedChanged, this, [this, vehicle](bool armed) {
        if (armed) {
            _startVehicle(vehicle);
        } else {
            (void) finish(vehicle->id());
        }
    });
    if (vehicle->armed()) {
        _startVehicle(vehicle);
    }
}

void GeoZoneFlightRecorder::_removeVehicle(Vehicle *vehicle)
{
    if (!vehicle) {
        return;
    }
    (void) disconnect(vehicle, nullptr, this, nullptr);
    if (_vehicles.value(vehicle->id()) == vehicle) {
        (void) finish(vehicle->id());
    }
}

void GeoZoneFlightRecorder::_startVehicle(Vehicle *vehicle)
{
    start(vehicle->id(), vehicle->vehicleTypeString(), firmwareText(vehicle));
    _vehicles.insert(vehicle->id(), vehicle);
    _sampleVehicles();
    _sampleTimer.start();
}

void GeoZoneFlightRecorder::_sampleVehicles()
{
    for (auto it = _vehicles.begin(); it != _vehicles.end();) {
        Vehicle *const vehicle = it.value();
        if (!vehicle) {
            it = _vehicles.erase(it);
            continue;
        }
        const QGeoCoordinate coordinate = vehicle->coordinate();
        if (coordinate.isValid()) {
            addPosition(it.key(), GeoZoneVehicleMonitor::vehiclePosition(vehicle, coordinate));
        }
        ++it;
    }
    if (_vehicles.isEmpty()) {
        _sampleTimer.stop();
    }
}

void GeoZoneFlightRecorder::_alertEvent(const GeoZoneMonitor::AlertEvent &event)
{
    const auto it = _flights.find(event.vehicleId);
    if (it != _flights.end()) {
        it->alerts.append(event);
    }
}

void GeoZoneFlightRecorder::start(int vehicleId, const QString &vehicle, const QString &firmware)
{
    if (recording(vehicleId)) {
        (void) finish(vehicleId);
    }
    Flight flight;
    flight.vehicleId = vehicleId;
    flight.vehicle = vehicle;
    flight.firmware = firmware;
    flight.start = GeoZoneEnvironment::currentDateTimeUtc();
    flight.plannedStart = _manager->planning()->plannedStart();
    _flights.insert(vehicleId, flight);
    qCDebug(GeoZoneMonitorLog) << "Recording the flight of vehicle" << vehicleId;
}

void GeoZoneFlightRecorder::addPosition(int vehicleId, const GeoZone::Position &position)
{
    const auto it = _flights.find(vehicleId);
    if ((it == _flights.end()) || !position.coordinate.isValid()) {
        return;
    }
    const Sample sample{ GeoZoneEnvironment::currentDateTimeUtc(), position };
    addSample(it->track, sample);
    it->latest = sample;
}

const GeoZoneFlightRecorder::Flight *GeoZoneFlightRecorder::flight(int vehicleId) const
{
    const auto it = _flights.constFind(vehicleId);
    return (it != _flights.cend()) ? &it.value() : nullptr;
}

void GeoZoneFlightRecorder::addSample(QList<Sample> &track, const Sample &sample)
{
    if (!track.isEmpty()) {
        const Sample &last = track.constLast();
        const double lastAltitude = last.position.altitudeAMSL;
        const double altitude = sample.position.altitudeAMSL;
        const bool moved = last.position.coordinate.distanceTo(sample.position.coordinate) >= sampleDistance;
        const bool climbed = (std::isnan(lastAltitude) != std::isnan(altitude)) || (std::abs(altitude - lastAltitude) >= sampleClimb);
        const bool due = last.time.secsTo(sample.time) >= sampleMaxSecs;
        if (!moved && !climbed && !due) {
            return;
        }
    }
    track.append(sample);
    if (track.count() > maxTrackPoints) {
        QList<Sample> thinned;
        thinned.reserve((track.count() / 2) + 1);
        for (qsizetype i = 0; i < track.count(); i++) {
            if (((i % 2) == 0) || (i == (track.count() - 1))) {
                thinned.append(track[i]);
            }
        }
        track = thinned;
    }
}

QString GeoZoneFlightRecorder::finish(int vehicleId)
{
    (void) _vehicles.remove(vehicleId);
    if (!recording(vehicleId)) {
        return QString();
    }
    Flight flight = _flights.take(vehicleId);
    flight.end = GeoZoneEnvironment::currentDateTimeUtc();
    if (flight.latest && (flight.track.isEmpty() || (flight.track.constLast().time != flight.latest->time))) {
        flight.track.append(*flight.latest);
    }

    // The flight is over either way: a record which can't be written is reported rather than lost silently
    const auto failed = [this, &flight](const QString &path, const QString &reason) {
        qCWarning(GeoZoneMonitorLog) << "Unable to write the geozone flight record of vehicle" << flight.vehicleId << path << reason;
        emit recordFailed(tr("The geozone record of the flight of vehicle %1 couldn't be written: %2").arg(flight.vehicleId).arg(reason));
        return QString();
    };
    const QString path = recordFilePath(flight);
    if (path.isEmpty()) {
        return failed(path, tr("no telemetry folder is set"));
    }
    (void) QDir().mkpath(QFileInfo(path).absolutePath());
    // A record is written each flight, a good time to delete those past the retention
    (void) removeOldRecords(flight.end.date(), retentionDays());
    QString errorString;
    if (!GeoZoneMissionExport::writeFile(path, QJsonDocument(record(flight)).toJson(QJsonDocument::Indented), errorString)) {
        return failed(path, errorString);
    }
    qCDebug(GeoZoneMonitorLog) << "Wrote the flight record" << path;
    emit recordWritten(path);
    return path;
}

QString GeoZoneFlightRecorder::directory() const
{
    if (!_directory.isEmpty()) {
        return _directory;
    }
    const QString telemetry = SettingsManager::instance()->appSettings()->telemetrySavePath();
    return telemetry.isEmpty() ? QString() : QDir(telemetry).filePath(QStringLiteral("GeoZones"));
}

QString GeoZoneFlightRecorder::recordFilePath(const Flight &flight) const
{
    const QString folder = directory();
    if (folder.isEmpty()) {
        return QString();
    }
    const QString name = QStringLiteral("GeoZoneFlight-%1-vehicle%2.json").arg(flight.start.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH-mm-ssZ")))
                                                                           .arg((flight.vehicleId == GeoZoneMonitor::noVehicle) ? 0 : flight.vehicleId);
    return QDir(folder).filePath(name);
}

int GeoZoneFlightRecorder::retentionDays()
{
    return GeoZoneSettings::instance()->recordRetention()->rawValue().toInt();
}

int GeoZoneFlightRecorder::removeOldRecords(const QDate &today, int days)
{
    const QString folder = directory();
    if (folder.isEmpty() || (days <= 0)) {
        return 0;
    }
    static const QRegularExpression namePattern(QStringLiteral("^GeoZoneFlight-(\\d{4}-\\d{2}-\\d{2})T.*\\.json$"));
    const QDate oldest = today.addDays(1 - days);
    int removed = 0;
    QDir dir(folder);
    for (const QString &name : dir.entryList({ QStringLiteral("GeoZoneFlight-*.json") }, QDir::Files)) {
        const QRegularExpressionMatch match = namePattern.match(name);
        const QDate date = match.hasMatch() ? QDate::fromString(match.captured(1), Qt::ISODate) : QDate();
        if (date.isValid() && (date < oldest) && dir.remove(name)) {
            removed++;
        }
    }
    if (removed > 0) {
        qCDebug(GeoZoneMonitorLog) << "Removed" << removed << "geozone flight records before" << oldest;
    }
    return removed;
}

QJsonObject GeoZoneFlightRecorder::alertJson(const QStringList &header, const QStringList &fields)
{
    static const QStringList numbers = { QStringLiteral("vehicle"), QStringLiteral("latitude"), QStringLiteral("longitude"), QStringLiteral("altitude_amsl_m"),
                                         QStringLiteral("terrain_amsl_m"), QStringLiteral("distance_m"), QStringLiteral("time_to_entry_s"),
                                         QStringLiteral("warning_distance_m"), QStringLiteral("warning_vertical_m"), QStringLiteral("warning_time_s"),
                                         QStringLiteral("ground_speed_mps"), QStringLiteral("vertical_speed_mps") };
    QJsonObject json;
    for (qsizetype i = 0; i < header.count(); i++) {
        const QString value = fields.value(i);
        if (value.isEmpty()) {
            json.insert(header[i], QJsonValue(QJsonValue::Null));
            continue;
        }
        bool ok = false;
        const double number = numbers.contains(header[i]) ? value.toDouble(&ok) : qQNaN();
        json.insert(header[i], ok ? QJsonValue(number) : QJsonValue(value));
    }
    return json;
}

QJsonArray GeoZoneFlightRecorder::loggedAlerts(int vehicleId, const QDateTime &start, const QDateTime &end) const
{
    QJsonArray rows;
    GeoZoneAlertHistory *const history = _monitor ? _monitor->alertHistory() : nullptr;
    if (!history || !start.isValid() || !end.isValid()) {
        return rows;
    }
    (void) history->flushLog();
    const QString vehicle = QString::number(vehicleId);
    for (QDate date = start.toUTC().date(); date <= end.toUTC().date(); date = date.addDays(1)) {
        QFile file(history->logFilePath(date));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        // Each file has its header, which may have columns added since
        QStringList header = GeoZoneAlertHistory::logHeader().split(QLatin1Char(','));
        QTextStream stream(&file);
        while (!stream.atEnd()) {
            const QString line = stream.readLine();
            if (line.isEmpty()) {
                continue;
            }
            const QStringList fields = GeoZoneAlertHistory::logFields(line);
            if (fields.constFirst() == header.constFirst()) {
                header = fields;
                continue;
            }
            const QDateTime time = QDateTime::fromString(fields.constFirst(), Qt::ISODateWithMs);
            if (!time.isValid() || (time < start) || (time > end) || (fields.value(header.indexOf(GeoZoneAlertHistory::logColumnName(GeoZoneAlertHistory::VehicleColumn))) != vehicle)) {
                continue;
            }
            rows.append(alertJson(header, fields));
        }
    }
    return rows;
}

QJsonObject GeoZoneFlightRecorder::record(const Flight &flight) const
{
    QList<GeoZone::Position> positions;
    QList<QGeoCoordinate> coordinates;
    double distance = 0;
    double maxAltitude = qQNaN();
    for (const Sample &sample : flight.track) {
        if (!coordinates.isEmpty()) {
            distance += coordinates.constLast().distanceTo(sample.position.coordinate);
        }
        positions.append(sample.position);
        coordinates.append(sample.position.coordinate);
        if (!std::isnan(sample.position.altitudeAMSL) && (std::isnan(maxAltitude) || (sample.position.altitudeAMSL > maxAltitude))) {
            maxAltitude = sample.position.altitudeAMSL;
        }
    }

    // The zones of the track flown, at the times it was flown
    GeoZoneBriefing::Input input;
    GeoZoneBriefing::Mission &mission = input.mission;
    mission.name = tr("Flight of vehicle %1").arg(flight.vehicleId);
    mission.vehicle = flight.vehicle;
    mission.firmware = flight.firmware;
    mission.plannedStart = flight.start;
    mission.plannedEnd = flight.end;
    mission.duration = static_cast<double>(flight.start.secsTo(flight.end));
    mission.distance = distance;
    mission.maxAltitudeAMSL = maxAltitude;
    if (!positions.isEmpty()) {
        GeoZonePathEvaluator::Path path = GeoZonePathEvaluator::Path::fromPositions(positions);
        for (qsizetype i = 0; i < path.vertices.count(); i++) {
            path.vertices[i].time = flight.track[i].time;
        }
        input.paths.append(path);
    }
    input.start = flight.start;
    input.end = flight.end;
    input.scheduleTolerance = 0;
    input.authorisations = _manager->planning()->authorisations();
    input.sources = GeoZoneSourceVersion::covering(_manager->repository()->sourceList(), GeoZoneBox::fromPath(coordinates));
    input.now = flight.end;
    GeoZoneBriefing briefing = GeoZoneBriefing::build(_manager->catalog(), input);
    briefing.operatorDetails = GeoZoneRemoteId::exportedOperatorDetails();
    QJsonObject json = GeoZoneBriefingExport::toJson(briefing);
    json.insert(QStringLiteral("schema"), QString::fromLatin1(jsonSchema));
    json.insert(QStringLiteral("version"), jsonVersion);
    (void) json.remove(QStringLiteral("mission"));

    QJsonObject flightJson;
    flightJson.insert(QStringLiteral("vehicleId"), (flight.vehicleId == GeoZoneMonitor::noVehicle) ? QJsonValue(QJsonValue::Null) : QJsonValue(flight.vehicleId));
    flightJson.insert(QStringLiteral("vehicle"), flight.vehicle);
    flightJson.insert(QStringLiteral("firmware"), flight.firmware);
    flightJson.insert(QStringLiteral("start"), jsonTime(flight.start));
    flightJson.insert(QStringLiteral("end"), jsonTime(flight.end));
    flightJson.insert(QStringLiteral("plannedStart"), jsonTime(flight.plannedStart));
    flightJson.insert(QStringLiteral("durationSeconds"), jsonNumber(mission.duration));
    flightJson.insert(QStringLiteral("distanceMeters"), distance);
    flightJson.insert(QStringLiteral("maxAltitudeAMSL"), jsonNumber(maxAltitude));
    flightJson.insert(QStringLiteral("takeoff"), flight.track.isEmpty() ? QJsonValue(QJsonValue::Null) : jsonPosition(flight.track.constFirst().position));
    flightJson.insert(QStringLiteral("landing"), flight.track.isEmpty() ? QJsonValue(QJsonValue::Null) : jsonPosition(flight.track.constLast().position));
    json.insert(QStringLiteral("flight"), flightJson);

    QJsonArray track;
    for (const Sample &sample : flight.track) {
        QJsonObject point = jsonPosition(sample.position).toObject();
        point.insert(QStringLiteral("time"), jsonTime(sample.time));
        track.append(point);
    }
    json.insert(QStringLiteral("track"), track);

    // The authorisations of the zones entered
    QJsonArray permits;
    for (const GeoZoneBriefing::Zone &zone : briefing.zones) {
        if ((zone.involvement != GeoZoneBriefing::Involvement::Entered) || !zone.authorisation) {
            continue;
        }
        permits.append(QJsonObject{
            { QStringLiteral("zoneKey"), zone.key },
            { QStringLiteral("identifier"), zone.identifier },
            { QStringLiteral("name"), zone.name },
            { QStringLiteral("permit"), zone.authorisation->permit },
            { QStringLiteral("validFrom"), jsonTime(zone.authorisation->validFrom) },
            { QStringLiteral("validTo"), jsonTime(zone.authorisation->validTo) },
            { QStringLiteral("coversFlight"), zone.authorisation->coversFlight },
        });
    }
    json.insert(QStringLiteral("permits"), permits);

    // The log has the pilot's answers to the alerts too, the alerts raised are recorded when it can't be read
    QJsonArray alerts = loggedAlerts(flight.vehicleId, flight.start, flight.end);
    if (alerts.isEmpty()) {
        const QStringList header = GeoZoneAlertHistory::logHeader().split(QLatin1Char(','));
        for (const GeoZoneMonitor::AlertEvent &event : flight.alerts) {
            alerts.append(alertJson(header, GeoZoneAlertHistory::logFields(GeoZoneAlertHistory::logLine(event))));
        }
    }
    json.insert(QStringLiteral("alerts"), alerts);
    return json;
}
