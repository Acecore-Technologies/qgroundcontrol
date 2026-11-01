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

#include <functional>

#include "GeoZone.h"
#include "GeoZoneCancelToken.h"
#include "GeoZonePathEvaluator.h"
#include "GeoZoneRouteSearch.h"

/// Climbs over the ceilings and descents below the floors of the zones a leg enters. Each round moves the start and
/// the end of the leg just beyond the limits of the zones it entered, where the leg passes them, and checks the leg
/// again, until it enters no zone or it enters zones whose limits aren't known.
class GeoZoneRouteVertical
{
public:
    /// @return Zones the path enters while they apply
    using Entered = std::function<QList<const GeoZone*>(const QList<GeoZone::Position> &path)>;

    /// @return Positions along the leg, Settings::sampleSpacing apart, at the altitudes the leg changes evenly through,
    ///         with the terrain elevation from the lookup when set
    static QList<GeoZone::Position> samples(const GeoZoneRouteSearch::Leg &leg, const GeoZoneRouteSearch::Settings &settings,
                                            const GeoZonePathEvaluator::TerrainLookup &terrainLookup);

    GeoZoneRouteVertical(const GeoZonePathEvaluator &evaluator, const GeoZoneRouteSearch::Settings &settings, const Entered &entered,
                         const GeoZoneCancelToken &cancel);

    /// @param samples See samples
    /// @param crossed Zones the leg enters
    /// @param zones   Set to the zones avoided, or to those preventing the route
    /// @return The climb or the descent, without zone names
    GeoZoneRouteSearch::Route route(GeoZoneRouteSearch::Kind kind, const QList<GeoZone::Position> &samples, const QList<const GeoZone*> &crossed,
                                    QList<const GeoZone*> &zones) const;

private:
    const GeoZonePathEvaluator &_evaluator;
    const GeoZoneRouteSearch::Settings &_settings;
    const Entered _entered;
    const GeoZoneCancelToken _cancel;
};
