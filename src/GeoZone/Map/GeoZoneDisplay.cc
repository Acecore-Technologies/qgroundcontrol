/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneDisplay.h"
#include "GeoZoneSettings.h"

#include <algorithm>

GeoZoneDisplay::GeoZoneDisplay(QObject *parent)
    : QObject(parent)
{
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    (void) connect(settings->showZones(), &Fact::rawValueChanged, this, [this]() { emit visibleChanged(visible()); });
    (void) connect(settings->hiddenRestrictions(), &Fact::rawValueChanged, this, &GeoZoneDisplay::hiddenRestrictionsChanged);
    for (Fact *const fact : { settings->hiddenRestrictions(), settings->altitudeFilter(), settings->altitudeFilterMargin(), settings->scheduleFilter(),
                              settings->scheduleFilterHours(), settings->mapSource() }) {
        (void) connect(fact, &Fact::rawValueChanged, this, &GeoZoneDisplay::filtersChanged);
    }
}

bool GeoZoneDisplay::visible() const
{
    return GeoZoneSettings::instance()->showZones()->rawValue().toBool();
}

void GeoZoneDisplay::setVisible(bool visible)
{
    GeoZoneSettings::instance()->showZones()->setRawValue(visible);
}

int GeoZoneDisplay::hiddenRestrictions() const
{
    return GeoZoneSettings::instance()->hiddenRestrictions()->rawValue().toInt();
}

void GeoZoneDisplay::setRestrictionVisible(int restriction, bool visible)
{
    const int bit = 1 << restriction;
    const int hidden = visible ? (hiddenRestrictions() & ~bit) : (hiddenRestrictions() | bit);
    GeoZoneSettings::instance()->hiddenRestrictions()->setRawValue(hidden);
}

GeoZoneMapFilter GeoZoneDisplay::filter() const
{
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    GeoZoneMapFilter filter;
    filter.hiddenRestrictions = hiddenRestrictions();
    filter.source = settings->mapSource()->rawValue().toString();
    filter.altitudes = settings->altitudeFilter()->rawValue().toBool();
    filter.altitudeMargin = settings->altitudeFilterMargin()->rawValue().toDouble();
    filter.schedule = static_cast<GeoZoneMapFilter::Schedule>(std::clamp(settings->scheduleFilter()->rawValue().toInt(),
                                                                         static_cast<int>(GeoZoneMapFilter::AnyTime), static_cast<int>(GeoZoneMapFilter::ActiveSoon)));
    filter.scheduleHours = settings->scheduleFilterHours()->rawValue().toInt();
    return filter;
}

void GeoZoneDisplay::resetFilters()
{
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    settings->hiddenRestrictions()->setRawValue(0);
    settings->altitudeFilter()->setRawValue(false);
    settings->scheduleFilter()->setRawValue(static_cast<int>(GeoZoneMapFilter::AnyTime));
    settings->mapSource()->setRawValue(QString());
}
