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

QString GeoZoneInfo::sourceText() const
{
    if (!_zone) {
        return QString();
    }
    const int count = _zone->sourceCount();
    if (count <= 1) {
        return _zone->sourceText();
    }
    return QCoreApplication::translate("GeoZoneInfo", "%1 · %n source(s)", "Source of a zone, number of sources publishing it", count).arg(_zone->sourceText());
}

QString GeoZoneInfo::sourcesText() const
{
    if (!_zone) {
        return QString();
    }
    QStringList texts{ _zone->sourceText() };
    for (const GeoZone::OtherSource &other : _zone->otherSources()) {
        texts.append(other.text);
    }
    return texts.join(QStringLiteral("\n"));
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
