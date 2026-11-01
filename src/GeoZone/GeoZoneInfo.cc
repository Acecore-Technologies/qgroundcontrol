/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneInfo.h"

GeoZoneInfo::GeoZoneInfo(const GeoZone *zone)
    : _zone(zone ? zone->sharedFromThis() : QSharedPointer<const GeoZone>())
{
}

const GeoZone *GeoZoneInfo::fromVariant(const QVariant &value)
{
    return value.canConvert<GeoZoneInfo>() ? value.value<GeoZoneInfo>().zone() : nullptr;
}

QList<const GeoZone*> GeoZoneInfo::fromVariantList(const QVariantList &values)
{
    QList<const GeoZone*> zones;
    for (const QVariant &value : values) {
        if (const GeoZone *const zone = fromVariant(value)) {
            zones.append(zone);
        }
    }
    return zones;
}
