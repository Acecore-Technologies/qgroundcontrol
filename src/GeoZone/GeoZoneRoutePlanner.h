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
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtPositioning/QGeoCoordinate>

#include <optional>

#include "GeoZone.h"
#include "GeoZoneMissionPath.h"

class GeoZoneManager;
class MissionController;

/// Suggests how the legs of a mission between waypoints can avoid the restricted zones they enter: a detour around
/// the zones, which can be inserted as waypoints, or staying below the floor of zones which don't reach the ground,
/// which can lower the waypoints of the leg when their altitudes and the terrain allow it.
class GeoZoneRoutePlanner : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("MissionController.h")

    Q_PROPERTY(MissionController*   missionController   READ missionController  WRITE setMissionController  NOTIFY missionControllerChanged FINAL)
    /// Maps with text, canApply when the suggestion can change the mission, and applyText for the button doing it
    Q_PROPERTY(QVariantList         suggestions         READ suggestions                                    NOTIFY suggestionsChanged FINAL)
    /// Suggestions are computed and follow mission and zone changes while active, like while they are shown
    Q_PROPERTY(bool                 active              READ active             WRITE setActive             NOTIFY activeChanged FINAL)
    Q_PROPERTY(double               margin              READ margin                                         CONSTANT FINAL)       ///< Meters, see detourMargin

public:
    explicit GeoZoneRoutePlanner(QObject *parent = nullptr);
    /// @param manager Zones to avoid, GeoZoneManager::instance() when null
    explicit GeoZoneRoutePlanner(GeoZoneManager *manager, QObject *parent = nullptr);

    static constexpr double detourMargin = 100;     ///< Meters kept from the zones
    static constexpr int maxDetourRounds = 4;       ///< Zones a detour enters are added to those it avoids this many times
    static constexpr double floorMargin = 10;       ///< Meters below zone floors that lowered legs keep
    static constexpr double minGroundClearance = 10;    ///< Meters above the terrain that lowered legs keep
    static constexpr double floorSampleSpacing = 10;    ///< Meters between the positions where lowered legs are checked
    static constexpr int maxFloorSamples = 5000;

    MissionController *missionController() const;
    void setMissionController(MissionController *missionController);
    QVariantList suggestions() const;
    bool active() const { return _active; }
    double margin() const { return detourMargin; }
    void setActive(bool active);

    /// Computes the suggestions for the current mission
    Q_INVOKABLE void update();

    /// Inserts the waypoints of a detour into the mission or lowers the waypoints of a leg, then updates the suggestions
    /// @return true if the mission changed
    Q_INVOKABLE bool applySuggestion(int index);

    /// @return Waypoints between from and to which keep the margin from the zones horizontally, nothing if an end is
    ///         within the margin of them. Empty when the straight leg already keeps the margin.
    static std::optional<QList<QGeoCoordinate>> detourAround(const QGeoCoordinate &from, const QGeoCoordinate &to, const QList<const GeoZone*> &zones,
                                                             double margin = detourMargin);

signals:
    void missionControllerChanged();
    void suggestionsChanged();
    void activeChanged();

private:
    struct Suggestion {
        QString text;
        int insertIndex = -1;               ///< Visual item index where the detour waypoints go, -1 without a detour
        QList<QGeoCoordinate> waypoints;
        QList<std::pair<int, double>> altitudes;    ///< Visual item indexes with their new altitude in the item's altitude mode
    };

    /// @return Zones accepted by the routing policy which the path enters while they are active, flown from start to end
    QList<const GeoZone*> _zonesOnPath(const QList<GeoZone::Position> &path, const QDateTime &start, const QDateTime &end) const;
    /// @return New altitudes of the leg's waypoints keeping it floorMargin below the floor of the zone and
    ///         minGroundClearance above the terrain, empty when that isn't possible or the altitudes aren't known
    QList<std::pair<int, double>> _altitudesBelowFloor(const GeoZoneMissionPath::Leg &leg, const GeoZone *zone) const;

    GeoZoneManager *_manager = nullptr;
    QPointer<MissionController> _missionController;
    QList<Suggestion> _suggestions;
    bool _active = false;
    QTimer _updateTimer;
};
