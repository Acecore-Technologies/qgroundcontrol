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
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>

#include <optional>

#include "GeoZone.h"
#include "GeoZoneMonitor.h"

class GeoZoneManager;
class Vehicle;

/// Records each flight of a vehicle, from arming to disarming, and writes its record as JSON in the GeoZones folder of
/// the telemetry folder: the geozone briefing of the track flown, see GeoZoneBriefingExport::toJson, with the flight,
/// its track, the permits used and the rows of the alert log of the vehicle during the flight, which include the
/// alerts and the pilot's answers to them. Without the alert log, the alerts the monitor raised are recorded.
///
/// The track keeps a position when the vehicle moved sampleDistance or climbed sampleClimb since the last one kept, or
/// sampleMaxSecs passed, and every other one when it grows beyond maxTrackPoints.
///
/// The alert logs and the records older than the retention of GeoZoneSettings::recordRetention are deleted when a
/// file is started, see removeOldRecords.
class GeoZoneFlightRecorder : public QObject
{
    Q_OBJECT

public:
    /// Records the flights of the vehicles of MultiVehicleManager, with the alerts of GeoZoneMonitor::instance()
    explicit GeoZoneFlightRecorder(QObject *parent = nullptr);
    /// Records the flights passed to start(), addPosition() and finish(), with the zones of the manager and the alerts
    /// of the monitor
    GeoZoneFlightRecorder(GeoZoneManager *manager, GeoZoneMonitor *monitor, QObject *parent = nullptr);

    static GeoZoneFlightRecorder *instance();

    static constexpr const char *jsonSchema = "qgc-geozone-flight-record";
    static constexpr int jsonVersion = 1;
    static constexpr int sampleIntervalMSecs = 1000;
    static constexpr double sampleDistance = 10;    ///< Meters
    static constexpr double sampleClimb = 5;        ///< Meters
    static constexpr int sampleMaxSecs = 30;
    static constexpr int maxTrackPoints = 2000;

    struct Sample {
        QDateTime time;                 ///< UTC
        GeoZone::Position position;
    };

    /// A flight being recorded
    struct Flight {
        int vehicleId = GeoZoneMonitor::noVehicle;
        QString vehicle;                ///< Vehicle type
        QString firmware;
        QDateTime start;                ///< UTC
        QDateTime end;
        QDateTime plannedStart;         ///< Of the planned flight when it started, invalid without
        QList<Sample> track;
        std::optional<Sample> latest;   ///< Last position, which the track gets at the end
        QList<GeoZoneMonitor::AlertEvent> alerts;   ///< Raised during the flight
    };

    /// Starts recording a flight of the vehicle, ending the one it may have
    void start(int vehicleId, const QString &vehicle = QString(), const QString &firmware = QString());
    /// Adds a position of the vehicle at the current time to its flight
    void addPosition(int vehicleId, const GeoZone::Position &position);
    /// Ends the flight of the vehicle and writes its record
    /// @return Path of the record written, empty without flight or on errors
    QString finish(int vehicleId);
    bool recording(int vehicleId) const { return _flights.contains(vehicleId); }
    /// @return Flight being recorded, null without
    const Flight *flight(int vehicleId) const;

    /// Folder of the records, the GeoZones folder of the telemetry folder when empty
    QString directory() const;
    void setDirectory(const QString &directory) { _directory = directory; }
    /// @return File of the record of the flight, empty without folder
    QString recordFilePath(const Flight &flight) const;
    /// Deletes the records of the folder of the days before the last ones, today included, none when days is 0
    /// @return Number of files deleted
    int removeOldRecords(const QDate &today, int days);
    /// @return Days the alert logs and flight records are kept, 0 to keep them, see GeoZoneSettings::recordRetention
    static int retentionDays();

    /// @return The record of the flight, see the class description
    QJsonObject record(const Flight &flight) const;
    /// @return The rows of the alert log about the vehicle from start to end, as objects of the columns of the log
    QJsonArray loggedAlerts(int vehicleId, const QDateTime &start, const QDateTime &end) const;
    /// @return Row of the alert log as an object of its columns, numbers for the coordinates, altitudes, distances and
    ///         times, empty ones null
    static QJsonObject alertJson(const QStringList &header, const QStringList &fields);

    /// Adds the sample to the track when it moved enough since the last one kept, see the class description
    static void addSample(QList<Sample> &track, const Sample &sample);

signals:
    void recordWritten(const QString &filePath);

private:
    void _addVehicle(Vehicle *vehicle);
    void _removeVehicle(Vehicle *vehicle);
    void _startVehicle(Vehicle *vehicle);
    void _sampleVehicles();
    void _alertEvent(const GeoZoneMonitor::AlertEvent &event);

    GeoZoneManager *_manager = nullptr;
    QPointer<GeoZoneMonitor> _monitor;
    QString _directory;
    QHash<int, Flight> _flights;                    ///< By vehicle id
    QHash<int, QPointer<Vehicle>> _vehicles;        ///< Flying vehicles by id, sampled every sampleIntervalMSecs
    QTimer _sampleTimer;
};
