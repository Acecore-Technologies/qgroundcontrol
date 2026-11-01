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
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

#include "GeoZone.h"
#include "GeoZoneIndex.h"

class GeoZoneShape;
class GeoZoneSource;

/// The zones and shapes of all sources with a spatial index, for the queries of maps, mission checks and alerts.
/// Rebuilt when the zones change, the zones and shapes belong to their sources.
class GeoZoneCatalog
{
public:
    /// Collects the zones of the sources in their order and indexes them
    void rebuild(const QList<const GeoZoneSource*> &sources);

    const QList<GeoZone*> &zones() const { return _zones; }
    const QList<GeoZoneShape*> &shapes() const { return _shapes; }
    QGeoRectangle boundingBox() const { return _boundingBox; }

    /// @return Shapes of the zones whose bounding box intersects the bounds in degrees
    QList<const GeoZoneShape*> shapesInArea(double south, double west, double north, double east) const;

    /// @return Zones centered in the region, limited to maxCount preferring the most restrictive
    QList<GeoZone*> labelZones(const QGeoRectangle &region, int maxCount) const;

    /// @return Zones whose horizontal projection contains the coordinate, the active and most restrictive first
    QList<GeoZone*> zonesAt(const QGeoCoordinate &coordinate) const;

    /// @return Zones whose name or identifier contains the text ignoring case, those starting with it first,
    ///         then the most restrictive, limited to maxResults
    QList<GeoZone*> search(const QString &text, int maxResults) const;

    /// See GeoZoneManager::zonesInArea
    QList<GeoZone*> zonesInArea(const QList<GeoZone::Restriction> &restrictions, const QGeoRectangle &area,
                                const QDateTime &activeFrom, const QDateTime &activeTo) const;

    /// See GeoZoneManager::zonesCrossingPaths
    QList<GeoZone*> zonesCrossingPaths(const QList<GeoZone::Restriction> &restrictions, const QList<QList<GeoZone::Position>> &paths) const;

    /// See GeoZoneManager::zonesIntersectingAreas
    QList<GeoZone*> zonesIntersectingAreas(const QList<GeoZone::Restriction> &restrictions, const QList<GeoZone::Area> &areas) const;

private:
    QList<GeoZone*> _zones;
    QList<GeoZoneShape*> _shapes;
    QList<QList<const GeoZoneShape*>> _zoneShapes;  ///< Shapes of each zone of _zones
    GeoZoneIndex _index;                            ///< Indexes into _zones
    QGeoRectangle _boundingBox;
};
