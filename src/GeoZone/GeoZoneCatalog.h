/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QList>
#include <QtCore/QSharedPointer>
#include <QtCore/QString>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

#include "GeoZone.h"
#include "GeoZoneBox.h"
#include "GeoZoneIndex.h"
#include "GeoZonePolicy.h"

class GeoZoneShape;
class GeoZoneSource;

/// The zones and shapes of all sources with a spatial index, for the queries of maps, mission checks and alerts.
/// Rebuilt when the zones change, the zones and shapes belong to their sources. The data is immutable and shared by
/// copies, which are cheap and keep the zones alive, so that another thread can query a copy while the sources change,
/// but not the shapes. Queries return const zones, since only their sources and the GUI thread modify them.
class GeoZoneCatalog
{
public:
    /// Collects the zones of the sources in their order and indexes them. Copies keep the previous zones.
    void rebuild(const QList<const GeoZoneSource*> &sources);

    /// Valid until the catalog is rebuilt or destroyed. Not const, so that the GUI thread can update GeoZone::active.
    const QList<GeoZone*> &zones() const { return _data->zones; }
    /// Valid until the catalog is rebuilt or destroyed
    const QList<GeoZoneShape*> &shapes() const { return _data->shapes; }
    /// Bounds of all zones, which may cross the antimeridian
    const GeoZoneBox &box() const { return _data->boundingBox; }
    QGeoRectangle boundingBox() const { return _data->boundingBox.toRectangle(); }

    /// @return Shapes of the zones whose bounding box intersects the bounds in degrees, see GeoZoneBox
    QList<const GeoZoneShape*> shapesInArea(double south, double west, double north, double east) const;

    /// @return Zones centered in the region, limited to maxCount preferring the most restrictive
    QList<const GeoZone*> labelZones(const QGeoRectangle &region, int maxCount) const;

    /// @return Zones whose horizontal projection contains the coordinate, the active and most restrictive first
    QList<const GeoZone*> zonesAt(const QGeoCoordinate &coordinate) const;

    /// @return Zones whose name or identifier contains the text ignoring case, those starting with it first,
    ///         then the most restrictive, limited to maxResults
    QList<const GeoZone*> search(const QString &text, int maxResults) const;

    /// @return Zones accepted by the policy whose bounding box intersects the area. An invalid area matches all zones.
    QList<const GeoZone*> zonesInArea(const GeoZoneBox &area, const GeoZonePolicy &policy) const;
    QList<const GeoZone*> zonesInArea(const QGeoRectangle &area, const GeoZonePolicy &policy) const { return zonesInArea(GeoZoneBox(area), policy); }

    /// First entry of a path into a zone
    struct PathHit {
        const GeoZone *zone = nullptr;
        int path = 0;               ///< Index of the path entering the zone
        GeoZone::PathEntry entry;
        double distance = 0;        ///< Meters along the path to the entry
    };

    /// @return First entry into each zone accepted by the policy which one of the paths enters, see
    ///         GeoZone::pathEntry, ordered by path and distance along it
    QList<PathHit> pathHits(const QList<QList<GeoZone::Position>> &paths, const GeoZonePolicy &policy) const;
    QList<PathHit> pathHits(const QList<GeoZone::Position> &path, const GeoZonePolicy &policy) const { return pathHits(QList<QList<GeoZone::Position>>{ path }, policy); }

    /// @return Zones accepted by the policy which overlap one of the areas, see GeoZone::intersectsArea
    QList<const GeoZone*> zonesIntersectingAreas(const QList<GeoZone::Area> &areas, const GeoZonePolicy &policy) const;

private:
    struct Data {
        QList<GeoZone*> zones;
        QList<QSharedPointer<GeoZone>> sharedZones;     ///< Keeps the zones of copies alive
        QList<GeoZoneShape*> shapes;
        QList<QList<const GeoZoneShape*>> zoneShapes;   ///< Shapes of each zone of zones
        GeoZoneIndex index;                             ///< Indexes into zones
        GeoZoneBox boundingBox;
    };

    QSharedPointer<const Data> _data = QSharedPointer<const Data>::create();
};
