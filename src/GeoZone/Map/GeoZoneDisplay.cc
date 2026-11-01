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

GeoZoneDisplay::GeoZoneDisplay(QObject *parent)
    : QObject(parent)
{
    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    (void) connect(settings->showZones(), &Fact::rawValueChanged, this, [this]() { emit visibleChanged(visible()); });
    (void) connect(settings->hiddenRestrictions(), &Fact::rawValueChanged, this, &GeoZoneDisplay::hiddenRestrictionsChanged);
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
