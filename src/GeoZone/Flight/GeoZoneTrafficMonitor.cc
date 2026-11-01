/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneTrafficMonitor.h"
#include "GeoZoneInfo.h"
#include "GeoZoneTerrain.h"

#include <QtCore/QVariantMap>
#include <QtCore/QtMath>

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

GeoZoneTrafficMonitor::GeoZoneTrafficMonitor(QObject *parent)
    : QObject(parent)
{
}

void GeoZoneTrafficMonitor::setEnabled(bool enabled)
{
    if (enabled == _enabled) {
        return;
    }
    _enabled = enabled;
    if (!enabled) {
        clear();
    }
    emit enabledChanged();
}

void GeoZoneTrafficMonitor::clear()
{
    _trackers.clear();
    _setZoneTraffic({});
}

QList<GeoZoneTrafficMonitor::Entry> GeoZoneTrafficMonitor::evaluate(const QList<Aircraft> &aircraft, const GeoZone::Position &center, double radius, const GeoZoneVehicleMonitor::Context &context)
{
    QList<Entry> entries;
    if (!_enabled || !center.coordinate.isValid() || !context.catalog) {
        clear();
        return entries;
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
    QList<ZoneTraffic> zoneTraffic;
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
        const GeoZoneAlertTracker::Update update = tracker.update(*context.catalog, context.policy, position, velocity, context.limits);
        for (const GeoZoneAlertTracker::Alert &alert : update.alerts) {
            if (alert.inside) {
                entries.append(Entry{ *plane, alert.zone, position });
            }
        }
        if (!update.nearby.isEmpty()) {
            zoneTraffic.append(ZoneTraffic{ *plane, update.nearby.constFirst() });
        }
        trackers.insert(plane->address, std::move(tracker));
    }
    _trackers = std::move(trackers);

    // Inside first, then the most restrictive zones, then the closest
    std::stable_sort(zoneTraffic.begin(), zoneTraffic.end(), [](const ZoneTraffic &a, const ZoneTraffic &b) {
        if (a.nearby.inside != b.nearby.inside) {
            return a.nearby.inside;
        }
        const int severityA = GeoZone::restrictionSeverity(a.nearby.zone->restriction());
        const int severityB = GeoZone::restrictionSeverity(b.nearby.zone->restriction());
        return (severityA != severityB) ? (severityA < severityB) : (a.nearby.distance < b.nearby.distance);
    });
    _setZoneTraffic(zoneTraffic);
    return entries;
}

void GeoZoneTrafficMonitor::forgetZones(const QString &sourceKey)
{
    QList<ZoneTraffic> zoneTraffic = _zoneTraffic;
    if (zoneTraffic.removeIf([&sourceKey](const ZoneTraffic &traffic) { return traffic.nearby.zone->sourceKey() == sourceKey; }) > 0) {
        _setZoneTraffic(zoneTraffic);
    }
}

void GeoZoneTrafficMonitor::_setZoneTraffic(const QList<ZoneTraffic> &zoneTraffic)
{
    if (zoneTraffic.isEmpty() && _zoneTraffic.isEmpty()) {
        return;
    }
    _zoneTraffic = zoneTraffic;
    emit aircraftChanged();
}

QVariantList GeoZoneTrafficMonitor::aircraft() const
{
    QVariantList aircraft;
    for (const ZoneTraffic &traffic : _zoneTraffic) {
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
