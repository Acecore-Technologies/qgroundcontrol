/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneActiveVehicleView.h"
#include "GeoZoneInfo.h"
#include "GeoZoneMonitor.h"
#include "GeoZoneNearbyModel.h"
#include "GeoZoneVehicleMonitor.h"

#include <algorithm>
#include <cmath>

GeoZoneActiveVehicleView::GeoZoneActiveVehicleView(QObject *parent)
    : QObject(parent)
    , _nearbyModel(new GeoZoneNearbyModel(this))
{
}

void GeoZoneActiveVehicleView::setMonitor(GeoZoneVehicleMonitor *monitor)
{
    if (monitor == _monitor) {
        return;
    }
    if (_monitor) {
        (void) _monitor->disconnect(this);
    }
    _monitor = monitor;
    if (monitor) {
        (void) connect(monitor, &GeoZoneVehicleMonitor::nearbyChanged, this, &GeoZoneActiveVehicleView::_publishNearby);
        (void) connect(monitor, &GeoZoneVehicleMonitor::verticalBandsChanged, this, &GeoZoneActiveVehicleView::verticalBandsChanged);
        (void) connect(monitor, &GeoZoneVehicleMonitor::predictionChanged, this, &GeoZoneActiveVehicleView::predictionChanged);
        (void) connect(monitor, &GeoZoneVehicleMonitor::positionUnavailableChanged, this, &GeoZoneActiveVehicleView::positionUnavailableChanged);
        (void) connect(monitor, &GeoZoneVehicleMonitor::terrainSourceChanged, this, &GeoZoneActiveVehicleView::altitudeDegradedChanged);
        (void) connect(monitor, &GeoZoneVehicleMonitor::fenceStatusChanged, this, &GeoZoneActiveVehicleView::fenceStatusChanged);
    }

    emit activeMonitorChanged();
    _publishNearby();
    emit verticalBandsChanged();
    emit predictionChanged();
    emit positionUnavailableChanged();
    emit altitudeDegradedChanged();
    emit fenceStatusChanged();
}

void GeoZoneActiveVehicleView::_publishNearby()
{
    _nearbyModel->setItems(_nearby());
    emit nearbyZonesChanged();
}

int GeoZoneActiveVehicleView::vehicleId() const
{
    return _monitor ? _monitor->vehicleId() : GeoZoneMonitor::noVehicle;
}

const QList<GeoZoneActiveVehicleView::Nearby> &GeoZoneActiveVehicleView::_nearby() const
{
    static const QList<Nearby> none;
    return _monitor ? _monitor->nearby() : none;
}

std::optional<GeoZoneAlertTracker::Entry> GeoZoneActiveVehicleView::_entry() const
{
    return _monitor ? _monitor->entry() : std::nullopt;
}

QVariantList GeoZoneActiveVehicleView::nearbyZones() const
{
    QVariantList zones;
    for (const Nearby &nearby : _nearby()) {
        zones.append(QVariantMap{
            { QStringLiteral("zone"), QVariant::fromValue(GeoZoneInfo(nearby.zone)) },
            { QStringLiteral("inside"), nearby.inside },
            { QStringLiteral("distance"), nearby.distance },
            { QStringLiteral("timeToEntry"), nearby.timeToEntry },
        });
    }
    return zones;
}

int GeoZoneActiveVehicleView::alertRestriction() const
{
    const QList<Nearby> &nearby = _nearby();
    return nearby.isEmpty() ? -1 : static_cast<int>(nearby.first().zone->restriction());
}

bool GeoZoneActiveVehicleView::inside() const
{
    const QList<Nearby> &nearby = _nearby();
    return std::any_of(nearby.cbegin(), nearby.cend(), [](const Nearby &zone) { return zone.inside; });
}

QVariantList GeoZoneActiveVehicleView::verticalBands() const
{
    QVariantList bands;
    if (!_monitor) {
        return bands;
    }
    for (const GeoZoneVerticalBand &band : _monitor->verticalBands()) {
        bands.append(QVariantMap{
            { QStringLiteral("restriction"), static_cast<int>(band.zone->restriction()) },
            { QStringLiteral("name"), band.zone->displayName() },
            { QStringLiteral("limitsText"), band.zone->verticalLimitsText() },
            { QStringLiteral("floor"), band.floor },
            { QStringLiteral("ceiling"), band.ceiling },
        });
    }
    return bands;
}

double GeoZoneActiveVehicleView::altitudeAMSL() const
{
    return _monitor ? _monitor->bandsPosition().altitudeAMSL : qQNaN();
}

double GeoZoneActiveVehicleView::terrainAMSL() const
{
    return _monitor ? _monitor->bandsPosition().terrainAMSL : qQNaN();
}

bool GeoZoneActiveVehicleView::positionUnavailable() const
{
    return _monitor && _monitor->positionUnavailable();
}

bool GeoZoneActiveVehicleView::altitudeDegraded() const
{
    return _monitor && _monitor->altitudeDegraded();
}

QVariantList GeoZoneActiveVehicleView::predictedPath() const
{
    const std::optional<GeoZoneAlertTracker::Entry> entry = _entry();
    if (!entry) {
        return {};
    }
    return { QVariant::fromValue(_monitor->entryFrom()), QVariant::fromValue(entry->position.coordinate) };
}

QGeoCoordinate GeoZoneActiveVehicleView::predictedEntry() const
{
    const std::optional<GeoZoneAlertTracker::Entry> entry = _entry();
    return entry ? entry->position.coordinate : QGeoCoordinate();
}

int GeoZoneActiveVehicleView::predictedEntryTime() const
{
    const std::optional<GeoZoneAlertTracker::Entry> entry = _entry();
    return entry ? static_cast<int>(std::lround(entry->time)) : -1;
}

QVariant GeoZoneActiveVehicleView::predictedEntryZone() const
{
    const std::optional<GeoZoneAlertTracker::Entry> entry = _entry();
    return entry ? QVariant::fromValue(GeoZoneInfo(entry->zone)) : QVariant();
}

bool GeoZoneActiveVehicleView::fenceBreached() const
{
    return _monitor && _monitor->fenceBreached();
}

QString GeoZoneActiveVehicleView::fenceBreachText() const
{
    return _monitor ? _monitor->fenceBreachText() : QString();
}

bool GeoZoneActiveVehicleView::fenceStatusKnown() const
{
    return _monitor && _monitor->fenceStatusKnown();
}
