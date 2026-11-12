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
#include <QtCore/QSharedPointer>
#include <QtCore/QString>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

#include <utility>

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
///
/// A zone which several sources publish, like a national file and a regional extract of it, is kept once: zones of a
/// later source with the identifier and country of a zone of an earlier source, and the same restriction, volumes and
/// schedule (see sameContent), are left out, and the kept zone lists their sources, see GeoZone::otherSources. Zones
/// with the same identifier and another content are all kept.
class GeoZoneCatalog
{
public:
    /// Collects the zones of the sources in their order, keeping zones several sources publish once, and indexes them.
    /// Copies keep the previous zones.
    void rebuild(const QList<const GeoZoneSource*> &sources);

    /// @return true if the zones have the same identifier and country, restriction, volumes and schedule: the same
    ///         published feature, or a copy of it which reads the same
    static bool sameContent(const GeoZone &a, const GeoZone &b);

    /// @return Number of zones left out because an earlier source publishes them, see GeoZone::otherSources
    int mergedZoneCount() const { return static_cast<int>(_data->merged.count()); }

    /// @return Numbers of zones of the source which other sources publish too, by the key of the other source
    QHash<QString, int> sharedZoneCounts(const QString &sourceKey) const;

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

    /// @return Zones whose name or identifier contains the text ignoring case and diacritics (see searchKey), those
    ///         starting with it first, then the most restrictive, limited to maxResults. A zone which overlapping
    ///         sources publish alike is found once, see rebuild. Zones of a source sharing their identifier, name and
    ///         restriction are zones of their own, each with its GeoZone::key, and are all found.
    QList<const GeoZone*> search(const QString &text, int maxResults) const;
    /// @return The text case folded without diacritics, like "zurich" for "Zürich", so that names match however
    ///         their accents are typed
    static QString searchKey(const QString &text);

    /// @return Zones accepted by the policy whose bounding box intersects the area. An invalid area matches all zones.
    QList<const GeoZone*> zonesInArea(const GeoZoneBox &area, const GeoZonePolicy &policy) const;
    QList<const GeoZone*> zonesInArea(const QGeoRectangle &area, const GeoZonePolicy &policy) const { return zonesInArea(GeoZoneBox(area), policy); }

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
        QList<std::pair<const GeoZone*, const GeoZone*>> merged;   ///< Zones left out, with the zone kept for them
    };

    QSharedPointer<const Data> _data = QSharedPointer<const Data>::create();
};
