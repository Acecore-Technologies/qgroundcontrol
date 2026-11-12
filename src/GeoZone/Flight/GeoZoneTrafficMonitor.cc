/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneTrafficMonitor.h"
#include "ADSBVehicle.h"
#include "ADSBVehicleManager.h"
#include "GeoZoneBox.h"
#include "GeoZoneCatalog.h"
#include "GeoZoneInfo.h"
#include "GeoZoneTerrain.h"
#include "QmlObjectListModel.h"

#include <QtCore/QVariantMap>
#include <QtCore/QtMath>
#include <QtPositioning/QGeoCircle>

#include <algorithm>
#include <cmath>
#include <utility>

QString GeoZoneTrafficMonitor::Aircraft::label() const
{
    // Callsigns are padded, some senders with null characters
    QString printable;
    for (const QChar character : callsign) {
        if (character.isPrint()) {
            printable.append(character);
        }
    }
    printable = printable.trimmed();
    return printable.isEmpty() ? QStringLiteral("%1").arg(address, 6, 16, QLatin1Char('0')).toUpper() : printable;
}

QList<GeoZoneTrafficMonitor::Aircraft> GeoZoneTrafficMonitor::adsbAircraft()
{
    const QmlObjectListModel *const adsbVehicles = ADSBVehicleManager::instance()->adsbVehicles();
    QList<Aircraft> aircraft;
    aircraft.reserve(adsbVehicles->count());
    for (int i = 0; i < adsbVehicles->count(); i++) {
        const ADSBVehicle *const adsbVehicle = adsbVehicles->value<const ADSBVehicle*>(i);
        if (!adsbVehicle) {
            continue;
        }
        Aircraft plane;
        plane.address = adsbVehicle->icaoAddress();
        plane.callsign = adsbVehicle->callsign();
        plane.coordinate = adsbVehicle->coordinate();
        plane.heading = adsbVehicle->heading();
        plane.speed = adsbVehicle->velocity();
        plane.verticalSpeed = adsbVehicle->verticalVel();
        aircraft.append(plane);
    }
    return aircraft;
}

QList<GeoZoneTrafficMonitor::Entry> GeoZoneTrafficMonitor::evaluate(const GeoZone::Position &center, const GeoZoneVehicleMonitor::Context &context,
                                                                    const QList<Aircraft> &aircraft, bool &changed)
{
    // The zones the aircraft may be in follow their schedules like those around the vehicles
    if (context.refreshActive) {
        context.refreshActive(GeoZoneBox(QGeoCircle(center.coordinate, radius).boundingGeoRectangle()));
    }

    // The closest aircraft within the radius
    QList<std::pair<double, const Aircraft*>> nearby;
    for (const Aircraft &candidate : aircraft) {
        if (!candidate.coordinate.isValid()) {
            continue;
        }
        const double distance = center.coordinate.distanceTo(candidate.coordinate);
        if (distance <= radius) {
            nearby.append({ distance, &candidate });
        }
    }
    std::sort(nearby.begin(), nearby.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    if (nearby.count() > maxAircraft) {
        nearby.resize(maxAircraft);
    }

    // Aircraft beyond the radius or gone are forgotten, so they alert again when they come back
    QHash<quint32, GeoZoneAlertTracker> trackers;
    QList<ZoneTraffic> traffic;
    QList<Entry> entries;
    for (const auto &[distance, plane] : std::as_const(nearby)) {
        Q_UNUSED(distance);
        GeoZoneAlertTracker tracker = _trackers.take(plane->address);
        // Without terrain data below the aircraft, the ground below the active vehicle is closer than none, which
        // would put aircraft at any height into zones limited above the ground
        GeoZone::Position position{ plane->coordinate, plane->coordinate.altitude(), GeoZoneTerrain::elevationAt(plane->coordinate) };
        if (std::isnan(position.terrainAMSL)) {
            position.terrainAMSL = center.terrainAMSL;
        }
        GeoZoneAlertTracker::Velocity velocity;
        if (!std::isnan(plane->heading) && !std::isnan(plane->speed)) {
            const double heading = qDegreesToRadians(plane->heading);
            velocity = GeoZoneAlertTracker::Velocity{ plane->speed * std::cos(heading), plane->speed * std::sin(heading), plane->verticalSpeed, true };
        }
        // The thresholds of the default profile at the aircraft's velocity
        const GeoZoneAlertThresholds::Result thresholds =
            GeoZoneAlertThresholds::compute(GeoZoneAlertThresholds::profile(QString::fromLatin1(GeoZoneAlertThresholds::defaultClass)), { velocity });
        const GeoZoneAlertTracker::Limits limits = context.limits.value_or(thresholds.limits());
        const GeoZoneAlertTracker::Update update = tracker.update(*context.catalog, context.policy, position, velocity, limits);
        for (const GeoZoneAlertTracker::Alert &zoneAlert : update.alerts) {
            if (zoneAlert.inside) {
                entries.append(Entry{ *plane, zoneAlert.zone, position, context.limits ? std::nullopt : std::optional<GeoZoneAlertThresholds::Result>(thresholds) });
            }
        }
        if (!update.nearby.isEmpty()) {
            traffic.append(ZoneTraffic{ *plane, update.nearby.constFirst() });
        }
        trackers.insert(plane->address, std::move(tracker));
    }
    _trackers = std::move(trackers);

    // Inside first, then the most restrictive zones, then the closest
    std::stable_sort(traffic.begin(), traffic.end(), [](const ZoneTraffic &a, const ZoneTraffic &b) {
        if (a.nearby.inside != b.nearby.inside) {
            return a.nearby.inside;
        }
        const int severityA = GeoZone::restrictionSeverity(a.nearby.zone->restriction());
        const int severityB = GeoZone::restrictionSeverity(b.nearby.zone->restriction());
        return (severityA != severityB) ? (severityA < severityB) : (a.nearby.distance < b.nearby.distance);
    });
    changed = _setTraffic(traffic);
    return entries;
}

bool GeoZoneTrafficMonitor::_setTraffic(const QList<ZoneTraffic> &traffic)
{
    if (traffic.isEmpty() && _traffic.isEmpty()) {
        return false;
    }
    _traffic = traffic;
    return true;
}

bool GeoZoneTrafficMonitor::clear()
{
    _trackers.clear();
    return _setTraffic({});
}

bool GeoZoneTrafficMonitor::forgetZones(const QString &sourceKey)
{
    QList<ZoneTraffic> traffic = _traffic;
    if (traffic.removeIf([&sourceKey](const ZoneTraffic &zoneTraffic) { return zoneTraffic.nearby.zone->sourceKey() == sourceKey; }) > 0) {
        return _setTraffic(traffic);
    }
    return false;
}

QVariantList GeoZoneTrafficMonitor::toVariantList() const
{
    QVariantList aircraft;
    for (const ZoneTraffic &traffic : _traffic) {
        aircraft.append(QVariantMap{
            { QStringLiteral("callsign"), traffic.aircraft.label() },
            { QStringLiteral("zone"), QVariant::fromValue(GeoZoneInfo(traffic.nearby.zone)) },
            { QStringLiteral("inside"), traffic.nearby.inside },
            { QStringLiteral("distance"), traffic.nearby.distance },
            { QStringLiteral("altitude"), traffic.aircraft.coordinate.altitude() },
        });
    }
    return aircraft;
}
