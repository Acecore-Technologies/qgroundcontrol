/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QVariantList>
#include <QtPositioning/QGeoCoordinate>

#include "GeoZone.h"
#include "GeoZoneAlertTracker.h"
#include "GeoZoneVehicleMonitor.h"

/// Checks ADS-B and Remote ID traffic against the geozones around the active vehicle, so the pilot sees which aircraft
/// are in or near zones. GeoZoneMonitor passes it the traffic of ADSBVehicleManager every evaluateIntervalMSecs while
/// it is turned on in GeoZoneSettings. Only the aircraft within the traffic radius of the active vehicle are evaluated,
/// each with its own approach and entry states, and those entering a zone are returned for low priority alerts.
class GeoZoneTrafficMonitor : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool         enabled     READ enabled    NOTIFY enabledChanged FINAL)    ///< Traffic is checked
    /// Aircraft in or near zones, the most restrictive zone and those inside first, as maps with callsign (or the
    /// ICAO address), zone (GeoZoneInfo of the most restrictive zone), inside, distance (meters to the zone) and altitude
    /// (meters AMSL, NaN when unknown)
    Q_PROPERTY(QVariantList aircraft    READ aircraft   NOTIFY aircraftChanged FINAL)
    Q_PROPERTY(int          count       READ count      NOTIFY aircraftChanged FINAL)

public:
    static constexpr int evaluateIntervalMSecs = 2000;
    static constexpr int maxAircraft = 100;     ///< Evaluated at a time, the closest to the active vehicle

    /// What is done when an aircraft enters a zone
    enum AlertMode {
        NoAlerts,
        ShowAlerts,     ///< Shown and logged, not spoken
        SpeakAlerts,    ///< Also spoken
    };

    struct Settings {
        bool enabled = false;
        double radius = 10000;              ///< Meters around the active vehicle
        AlertMode alerts = ShowAlerts;
    };

    /// An aircraft of the traffic
    struct Aircraft {
        quint32 address = 0;                ///< ICAO address, identifies the aircraft
        QString callsign;
        QGeoCoordinate coordinate;          ///< With the altitude AMSL when known
        double heading = qQNaN();           ///< Degrees
        double speed = qQNaN();             ///< Meters per second over ground
        double verticalSpeed = 0;           ///< Meters per second, up positive

        /// @return Callsign, or the ICAO address without
        QString label() const;
    };

    /// Aircraft in or near a zone, with the most restrictive one
    struct ZoneTraffic {
        Aircraft aircraft;
        GeoZoneAlertTracker::Nearby nearby;
    };

    /// Aircraft which entered a zone
    struct Entry {
        Aircraft aircraft;
        const GeoZone *zone = nullptr;
        GeoZone::Position position;
    };

    explicit GeoZoneTrafficMonitor(QObject *parent = nullptr);

    bool enabled() const { return _enabled; }
    /// Turned off, the aircraft and their states are forgotten
    void setEnabled(bool enabled);

    /// Evaluates the aircraft within the radius of the center against the zones of the context, forgetting those
    /// beyond it or gone. The terrain below the center is used for aircraft without terrain data below them.
    /// @return Aircraft which entered a zone since the last evaluation
    QList<Entry> evaluate(const QList<Aircraft> &aircraft, const GeoZone::Position &center, double radius, const GeoZoneVehicleMonitor::Context &context);
    /// Forgets the aircraft and their states
    void clear();
    /// Forgets the zones of a source which are about to be deleted, keeping their alert states
    void forgetZones(const QString &sourceKey);

    const QList<ZoneTraffic> &zoneTraffic() const { return _zoneTraffic; }
    QVariantList aircraft() const;
    int count() const { return static_cast<int>(_zoneTraffic.count()); }

signals:
    void enabledChanged();
    void aircraftChanged();

private:
    void _setZoneTraffic(const QList<ZoneTraffic> &zoneTraffic);

    bool _enabled = false;
    QHash<quint32, GeoZoneAlertTracker> _trackers;  ///< By ICAO address
    QList<ZoneTraffic> _zoneTraffic;
};
