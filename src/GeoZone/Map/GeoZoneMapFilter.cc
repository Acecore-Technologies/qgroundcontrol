/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMapFilter.h"

void GeoZoneAltitudeRange::add(const GeoZone::Position &position)
{
    if (std::isnan(position.altitudeAMSL)) {
        return;
    }
    // fmin and fmax ignore the NaN of an empty range
    minAMSL = std::fmin(minAMSL, position.altitudeAMSL);
    maxAMSL = std::fmax(maxAMSL, position.altitudeAMSL);
    if (!std::isnan(position.terrainAMSL)) {
        const double heightAboveTerrain = position.altitudeAMSL - position.terrainAMSL;
        minAGL = std::fmin(minAGL, heightAboveTerrain);
        maxAGL = std::fmax(maxAGL, heightAboveTerrain);
    }
}

bool GeoZoneAltitudeRange::near(const GeoZoneAltitudeRange &other, double tolerance) const
{
    const auto close = [tolerance](double a, double b) {
        return (std::isnan(a) && std::isnan(b)) || (std::abs(a - b) < tolerance);
    };
    return close(minAMSL, other.minAMSL) && close(maxAMSL, other.maxAMSL) && close(minAGL, other.minAGL) && close(maxAGL, other.maxAGL);
}

bool GeoZoneMapFilter::isActive() const
{
    return (hiddenRestrictions != 0) || !source.isEmpty() || altitudes || (schedule != AnyTime);
}

bool GeoZoneMapFilter::accepts(const GeoZone &zone, const QDateTime &now, const GeoZoneAltitudeRange &altitudeRange) const
{
    if ((hiddenRestrictions & (1 << zone.restriction())) || (!source.isEmpty() && (zone.sourceKey() != source))) {
        return false;
    }

    switch (schedule) {
    case AnyTime:
        break;
    case ActiveNow:
        if (!zone.active()) {
            return false;
        }
        break;
    case ActiveSoon:
        if (!zone.active() && !zone.isActiveDuring(now, now.addSecs(qint64{ scheduleHours } * 3600))) {
            return false;
        }
        break;
    }

    return !altitudes || nearAltitudes(zone, altitudeRange, altitudeMargin);
}

bool GeoZoneMapFilter::nearAltitudes(const GeoZone &zone, const GeoZoneAltitudeRange &range, double margin)
{
    const QList<GeoZone::Volume> &volumes = zone.data().volumes;
    if (!range.isValid() || volumes.isEmpty()) {
        return true;
    }

    // The terrain elevations give limits above ground in the heights above the terrain of the range
    const GeoZone::Position highest{ zone.center(), range.maxAMSL, range.maxAMSL - range.maxAGL };
    const GeoZone::Position lowest{ zone.center(), range.minAMSL, range.minAMSL - range.minAGL };
    for (const GeoZone::Volume &volume : volumes) {
        const double floor = GeoZone::limitAMSL(volume, false, highest);
        const double ceiling = GeoZone::limitAMSL(volume, true, lowest);
        if ((std::isnan(floor) || (floor <= (range.maxAMSL + margin))) && (std::isnan(ceiling) || (ceiling >= (range.minAMSL - margin)))) {
            return true;
        }
    }
    return false;
}
