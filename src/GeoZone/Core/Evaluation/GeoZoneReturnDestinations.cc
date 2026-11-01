/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneReturnDestinations.h"

namespace {

/// The position at the coordinate without its altitude, which is the altitude AMSL given
GeoZone::Position positionOf(const QGeoCoordinate &coordinate, double altitudeAMSL)
{
    return GeoZone::Position{ coordinate.isValid() ? QGeoCoordinate(coordinate.latitude(), coordinate.longitude()) : QGeoCoordinate(), altitudeAMSL };
}

GeoZone::Position positionOf(const GeoZone::Position &position)
{
    return positionOf(position.coordinate, position.altitudeAMSL);
}

} // namespace

GeoZoneReturnModel::Destinations GeoZoneReturnDestinations::fromMission(const QList<GeoZoneMissionItem> &items, const GeoZone::Position &home,
                                                                        const QList<GeoZone::Position> &rallyPoints, const GeoZone::Position &breachReturnPoint)
{
    using Kind = GeoZoneMissionItem::Kind;
    GeoZoneReturnModel::Destinations destinations;
    if (home.coordinate.isValid()) {
        destinations.home = positionOf(home);
    }
    for (const GeoZone::Position &rallyPoint : rallyPoints) {
        if (rallyPoint.coordinate.isValid()) {
            destinations.rallyPoints.append(positionOf(rallyPoint));
        }
    }
    if (breachReturnPoint.coordinate.isValid()) {
        destinations.breachReturnPoint = positionOf(breachReturnPoint);
    }

    // Returning along the mission in reverse, PX4 lands at home once it reaches a takeoff instead of flying to it, see
    // RtlMissionFastReverse::setActiveMissionItems
    for (const GeoZoneMissionItem &item : items) {
        if (item.flies && (item.kind != GeoZoneMissionItem::Kind::Takeoff)) {
            destinations.firstWaypoint = positionOf(item.entry);
            break;
        }
    }

    // Landing sequences from their DO_LAND_START, and the return path after a DO_RETURN_PATH_START
    bool onReturnPath = false;
    for (qsizetype i = 0; i < items.count(); i++) {
        const GeoZoneMissionItem &item = items[i];
        if (item.kind == Kind::LandingPattern) {
            GeoZoneReturnModel::Landing landing;
            landing.entry = positionOf(item.entry);
            landing.touchdown = positionOf(item.exit);
            landing.marker = landing.entry.coordinate;
            landing.sequenceNumber = item.sequenceNumber;
            if (landing.entry.coordinate.isValid() && landing.touchdown.coordinate.isValid()) {
                destinations.landings.append(landing);
            }
            onReturnPath = false;
            continue;
        }
        if (item.kind == Kind::Pattern) {
            continue;
        }
        if (item.kind == Kind::LandStart) {
            GeoZoneReturnModel::Landing landing;
            landing.sequenceNumber = item.sequenceNumber;
            if (item.marker.isValid() && ((item.marker.latitude() != 0) || (item.marker.longitude() != 0))) {
                landing.marker = QGeoCoordinate(item.marker.latitude(), item.marker.longitude());
            }
            for (qsizetype j = i + 1; j < items.count(); j++) {
                const GeoZoneMissionItem &next = items[j];
                if (!next.flies) {
                    continue;
                }
                if (!landing.entry.coordinate.isValid()) {
                    landing.entry = positionOf(next.entry);
                }
                if (next.land) {
                    landing.touchdown = positionOf(next.exit);
                    break;
                }
            }
            if (landing.entry.coordinate.isValid() && landing.touchdown.coordinate.isValid()) {
                if (!landing.marker.isValid()) {
                    landing.marker = landing.entry.coordinate;
                }
                destinations.landings.append(landing);
            }
            onReturnPath = false;
        } else if (item.kind == Kind::ReturnPathStart) {
            onReturnPath = true;
        } else if (onReturnPath && item.flies) {
            if (item.land) {
                onReturnPath = false;
            } else {
                destinations.returnPath.append(positionOf(item.entry));
            }
        }
    }
    return destinations;
}
