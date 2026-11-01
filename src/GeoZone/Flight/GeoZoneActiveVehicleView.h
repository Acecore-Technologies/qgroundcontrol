/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QVariantList>
#include <QtPositioning/QGeoCoordinate>

#include "GeoZoneAlertTracker.h"

#include <optional>

Q_MOC_INCLUDE("GeoZoneNearbyModel.h")

class GeoZoneNearbyModel;
class GeoZoneVehicleMonitor;

/// The geozone state of the active vehicle for QML: its nearby zones, the zones above and below it, its predicted entry
/// and its fence status. GeoZoneMonitor points it at the GeoZoneVehicleMonitor of the active vehicle, and it follows
/// that monitor until it is pointed at another one, so QML binds to one object whatever vehicle is active.
class GeoZoneActiveVehicleView : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool         valid               READ valid              NOTIFY activeMonitorChanged FINAL)    ///< A vehicle or position is monitored
    Q_PROPERTY(int          vehicleId           READ vehicleId          NOTIFY activeMonitorChanged FINAL)    ///< System id, GeoZoneMonitor::noVehicle without
    Q_PROPERTY(QVariantList nearbyZones         READ nearbyZones        NOTIFY nearbyZonesChanged FINAL)      ///< Maps with zone, inside and distance (meters), most restrictive first
    Q_PROPERTY(int          alertRestriction    READ alertRestriction   NOTIFY nearbyZonesChanged FINAL)      ///< Most restrictive GeoZone::Restriction nearby, -1 without
    Q_PROPERTY(bool         inside              READ inside             NOTIFY nearbyZonesChanged FINAL)      ///< The vehicle is inside one of the nearby zones
    Q_PROPERTY(GeoZoneNearbyModel *nearbyModel  READ nearbyModel        CONSTANT FINAL)                       ///< nearbyZones as a list model
    /// Volumes of the active zones above and below the vehicle, as maps with restriction, name, limitsText, floor and
    /// ceiling (meters AMSL, NaN when the zone extends to the ground or has no ceiling, or the limit is unknown)
    Q_PROPERTY(QVariantList verticalBands       READ verticalBands      NOTIFY verticalBandsChanged FINAL)
    Q_PROPERTY(double       altitudeAMSL        READ altitudeAMSL       NOTIFY verticalBandsChanged FINAL)    ///< NaN when unknown
    Q_PROPERTY(double       terrainAMSL         READ terrainAMSL        NOTIFY verticalBandsChanged FINAL)    ///< Below the vehicle, NaN when unknown
    /// The vehicle has no current position: its alerts are paused and the nearby zones and bands are those of its last
    /// position, so they are stale
    Q_PROPERTY(bool         positionUnavailable READ positionUnavailable NOTIFY positionUnavailableChanged FINAL)
    /// Where the vehicle enters a nearby zone first at its current velocity within the approach time: the path from the
    /// vehicle to the entry, the entry position, the seconds until then and the zone (GeoZoneInfo). Empty, invalid and
    /// -1 without such an entry.
    Q_PROPERTY(QVariantList     predictedPath       READ predictedPath      NOTIFY predictionChanged FINAL)
    Q_PROPERTY(QGeoCoordinate   predictedEntry      READ predictedEntry     NOTIFY predictionChanged FINAL)
    Q_PROPERTY(int              predictedEntryTime  READ predictedEntryTime NOTIFY predictionChanged FINAL)
    Q_PROPERTY(QVariant         predictedEntryZone  READ predictedEntryZone NOTIFY predictionChanged FINAL)
    /// The vehicle reports a breach of its own fence (FENCE_STATUS), with the kind of breach and their count
    Q_PROPERTY(bool             fenceBreached       READ fenceBreached      NOTIFY fenceStatusChanged FINAL)
    Q_PROPERTY(QString          fenceBreachText     READ fenceBreachText    NOTIFY fenceStatusChanged FINAL)
    Q_PROPERTY(bool             fenceStatusKnown    READ fenceStatusKnown   NOTIFY fenceStatusChanged FINAL)     ///< The vehicle sends FENCE_STATUS

public:
    explicit GeoZoneActiveVehicleView(QObject *parent = nullptr);

    /// Follows the monitor, null for none. Notifies all properties when it changes.
    void setMonitor(GeoZoneVehicleMonitor *monitor);
    GeoZoneVehicleMonitor *monitor() const { return _monitor; }

    bool valid() const { return !_monitor.isNull(); }
    int vehicleId() const;
    QVariantList nearbyZones() const;
    int alertRestriction() const;
    bool inside() const;
    GeoZoneNearbyModel *nearbyModel() const { return _nearbyModel; }
    QVariantList verticalBands() const;
    double altitudeAMSL() const;
    double terrainAMSL() const;
    bool positionUnavailable() const;
    QVariantList predictedPath() const;
    QGeoCoordinate predictedEntry() const;
    int predictedEntryTime() const;
    QVariant predictedEntryZone() const;
    bool fenceBreached() const;
    QString fenceBreachText() const;
    bool fenceStatusKnown() const;

signals:
    /// Another vehicle became active, or none, all properties may have changed
    void activeMonitorChanged();
    void nearbyZonesChanged();
    void verticalBandsChanged();
    void positionUnavailableChanged();
    void predictionChanged();
    void fenceStatusChanged();

private:
    using Nearby = GeoZoneAlertTracker::Nearby;

    const QList<Nearby> &_nearby() const;
    std::optional<GeoZoneAlertTracker::Entry> _entry() const;
    void _publishNearby();

    QPointer<GeoZoneVehicleMonitor> _monitor;
    GeoZoneNearbyModel *_nearbyModel = nullptr;
};
