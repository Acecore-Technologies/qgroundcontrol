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
#include <QtCore/QString>
#include <QtCore/QVariantList>
#include <QtPositioning/QGeoCoordinate>

#include "GeoZone.h"
#include "GeoZoneAlertThresholds.h"
#include "GeoZoneAlertTracker.h"
#include "GeoZoneVehicleMonitor.h"

#include <optional>

/// The ADS-B and Remote ID traffic around the active vehicle checked against the zones, for GeoZoneMonitor: a
/// GeoZoneAlertTracker per aircraft tells which aircraft are in or near zones, for the pilot to see, and which entered
/// one, which the monitor alerts.
class GeoZoneTrafficMonitor
{
public:
    static constexpr int maxAircraft = 100;     ///< Checked at a time, the closest to the active vehicle
    static constexpr double radius = 10000;     ///< Meters around the active vehicle within which the traffic is checked

    /// An aircraft of the ADS-B or Remote ID traffic
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

    /// An aircraft which entered a zone
    struct Entry {
        Aircraft aircraft;
        const GeoZone *zone = nullptr;
        GeoZone::Position position;         ///< Of the aircraft, with the terrain below it or else below the vehicle
        /// Of the default profile at the velocity of the aircraft, nothing while the limits are fixed
        std::optional<GeoZoneAlertThresholds::Result> thresholds;
    };

    /// @return The aircraft of ADSBVehicleManager
    static QList<Aircraft> adsbAircraft();

    /// Checks the aircraft within the radius of the center, where the active vehicle is, against the zones of the
    /// context, forgetting those beyond it or gone, so that they alert again when they come back. The terrain below
    /// the center is used for aircraft without terrain data below them.
    /// @param changed Set to true when traffic() is to be notified
    /// @return The aircraft which entered a zone, in the order checked: the closest first
    QList<Entry> evaluate(const GeoZone::Position &center, const GeoZoneVehicleMonitor::Context &context, const QList<Aircraft> &aircraft, bool &changed);
    /// Forgets the aircraft and their states
    /// @return true when traffic() is to be notified
    bool clear();
    /// Forgets the aircraft in or near zones of the source, which are about to be deleted
    /// @return true when traffic() is to be notified
    bool forgetZones(const QString &sourceKey);

    /// Aircraft in or near zones: those inside first, then the most restrictive zones, then the closest
    const QList<ZoneTraffic> &traffic() const { return _traffic; }
    /// @return traffic() as maps with callsign (or the ICAO address), zone (GeoZoneInfo of the most restrictive zone),
    ///         inside, distance (meters to the zone) and altitude (meters AMSL, NaN when unknown)
    QVariantList toVariantList() const;

private:
    bool _setTraffic(const QList<ZoneTraffic> &traffic);

    QHash<quint32, GeoZoneAlertTracker> _trackers;  ///< By ICAO address
    QList<ZoneTraffic> _traffic;
};
