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
#include <QtCore/QObject>
#include <QtCore/QString>

#include <cmath>

#include "GeoZone.h"

/// Altitudes of a flight which the altitude filter of the maps compares the vertical limits of zones with: the
/// altitude of the active vehicle, or the range of altitudes of a planned mission
struct GeoZoneAltitudeRange
{
    double minAMSL = qQNaN();
    double maxAMSL = qQNaN();
    double minAGL = qQNaN();    ///< Height above the terrain, NaN when the terrain is unknown
    double maxAGL = qQNaN();

    bool isValid() const { return !std::isnan(minAMSL) && !std::isnan(maxAMSL); }
    /// Extends the range with the altitude of the position, and its height above the terrain when that is known.
    /// Positions without altitude are ignored.
    void add(const GeoZone::Position &position);
    /// @return true if the altitudes differ by less than tolerance meters, unknown ones being equal
    bool near(const GeoZoneAltitudeRange &other, double tolerance) const;
};

/// Which zones the maps show, see GeoZoneDisplay::filter. Filters only hide zones on the maps: the mission check,
/// flight alerts and guided command checks apply to all zones.
struct GeoZoneMapFilter
{
    enum Schedule {
        AnyTime,        ///< Zones whatever their schedule
        ActiveNow,      ///< Zones active now, see GeoZone::active
        ActiveSoon,     ///< Zones active now or within scheduleHours
        ActiveAtFlightTime, ///< Zones active from flightStart to flightEnd, widened by scheduleToleranceSecs, or now
                            ///< without flightStart
    };

    static constexpr double altitudeMargin = 100;   ///< Meters from the altitudes of the flight within which zones are near them
    static constexpr int scheduleHours = 12;        ///< Hours ahead in which zones active soon become active
    static constexpr int scheduleToleranceSecs = 15 * 60;   ///< Like GeoZonePathEvaluator::defaultScheduleToleranceSecs

    int hiddenRestrictions = 0;     ///< Bit mask, 1 << GeoZone::Restriction
    QString source;                 ///< Key of the only source shown, empty for all
    bool altitudes = false;         ///< Only zones near the altitudes of the flight
    Schedule schedule = AnyTime;
    QDateTime flightStart;          ///< Of the planned flight, see GeoZonePlanning::plannedStart
    QDateTime flightEnd;

    /// @return true if the filters can hide zones
    bool isActive() const;

    /// @return true if the zone passes the filters
    /// @param now UTC time from which ActiveSoon looks ahead
    /// @param altitudeRange Of the flight, all zones pass the altitude filter without it
    bool accepts(const GeoZone &zone, const QDateTime &now, const GeoZoneAltitudeRange &altitudeRange) const;

    /// @return true if the floor of a volume of the zone is at most altitudeMargin above the highest altitudes and its
    ///         ceiling at least altitudeMargin below the lowest ones. Limits above ground are compared with the heights
    ///         above the terrain, the others with the altitudes. Limits which can't be compared, like limits above
    ///         ground without the heights above the terrain, don't hide the zone.
    static bool nearAltitudes(const GeoZone &zone, const GeoZoneAltitudeRange &range);

    bool operator==(const GeoZoneMapFilter &other) const = default;
};

/// Which geozones the maps draw: all of them or none, and the filters hiding zones by restriction, altitude, schedule
/// and source, see GeoZoneMapFilter, shown by GeoZoneLegend. The mission check, flight alerts and guided command checks
/// apply to all zones. Kept in GeoZoneSettings.
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
    /// A filter changed
    void filtersChanged();
};
