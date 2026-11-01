/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QObject>

#include "GeoZoneMapFilter.h"

/// Which geozones the maps draw: all of them or none, and the filters hiding zones by restriction, altitude, schedule
/// and source, see GeoZoneMapFilter. The mission check, flight alerts and guided command checks apply to all zones.
/// Kept in GeoZoneSettings.
class GeoZoneDisplay : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool visible             READ visible            WRITE setVisible    NOTIFY visibleChanged FINAL)
    Q_PROPERTY(int  hiddenRestrictions  READ hiddenRestrictions                     NOTIFY hiddenRestrictionsChanged FINAL)   ///< Bit mask of the restrictions not drawn on maps, 1 << GeoZone::Restriction
    Q_PROPERTY(bool filtered            READ filtered                               NOTIFY filtersChanged FINAL)              ///< Filters can hide zones

public:
    explicit GeoZoneDisplay(QObject *parent = nullptr);

    bool visible() const;
    void setVisible(bool visible);
    int hiddenRestrictions() const;
    /// @return true if zones with the restriction are drawn on maps
    Q_INVOKABLE bool restrictionVisible(int restriction) const { return (hiddenRestrictions() & (1 << restriction)) == 0; }
    /// Shows or hides the zones with the restriction on maps
    Q_INVOKABLE void setRestrictionVisible(int restriction, bool visible);

    /// @return The filters of the settings
    GeoZoneMapFilter filter() const;
    bool filtered() const { return filter().isActive(); }
    /// Turns the filters off, so that the maps show all zones of all restrictions, altitudes, schedules and sources
    Q_INVOKABLE void resetFilters();

signals:
    void visibleChanged(bool visible);
    void hiddenRestrictionsChanged();
    /// A filter or its margin or hours changed
    void filtersChanged();
};
