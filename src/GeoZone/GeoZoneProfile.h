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

#include "GeoZone.h"

#include <cmath>

class GeoZoneManager;
class MissionController;

/// Vertical extent of the restricted geozones and U-space airspaces along the planned flight path, for the
/// altitude profile of the Plan View. Distances follow the mission items like TerrainProfile.
class GeoZoneProfile : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("MissionController.h")

    Q_PROPERTY(MissionController*   missionController   READ missionController  WRITE setMissionController  NOTIFY missionControllerChanged FINAL)
    /// One map per zone volume and stretch of the path inside its horizontal projection: restriction (GeoZone::Restriction),
    /// name, and points, a list of maps with distance (meters from the start), floor and ceiling (meters AMSL, NaN when
    /// the zone extends to the ground or has no ceiling, or the limit is unknown)
    Q_PROPERTY(QVariantList         bands               READ bands                                          NOTIFY bandsChanged FINAL)
    /// The bands are only computed while active, like while the profile is shown
    Q_PROPERTY(bool                 active              READ active             WRITE setActive             NOTIFY activeChanged FINAL)

public:
    explicit GeoZoneProfile(QObject *parent = nullptr);
    /// @param manager Zones to show, GeoZoneManager::instance() when null
    explicit GeoZoneProfile(GeoZoneManager *manager, QObject *parent = nullptr);

    static constexpr double minSampleSpacing = 5;   ///< Meters between samples along short paths
    static constexpr int maxSamples = 20000;        ///< About the samples along the whole path, which sets the spacing of long paths
    static constexpr double projectionStretch = 2000;   ///< Meters of a segment sharing a flat projection of the zones

    MissionController *missionController() const;
    void setMissionController(MissionController *missionController);

    QVariantList bands() const { return _bands; }
    bool active() const { return _active; }
    void setActive(bool active);

    /// Flight path segment between two mission items, in path order
    struct Segment {
        QGeoCoordinate from;
        QGeoCoordinate to;
        double distance = 0;        ///< Meters along the profile, which can differ slightly from the great circle
        QList<double> terrainAMSL;  ///< Ground elevation from the start to the end at even spacing, empty when unknown
        double terrainSpacing = 0;
    };

    struct BandPoint {
        double distance = 0;
        double floor = qQNaN();
        double ceiling = qQNaN();

        /// NaN limits compare equal, so unchanged bands are recognized
        friend bool operator==(const BandPoint &a, const BandPoint &b)
        {
            const auto same = [](double x, double y) { return (x == y) || (std::isnan(x) && std::isnan(y)); };
            return (a.distance == b.distance) && same(a.floor, b.floor) && same(a.ceiling, b.ceiling);
        }
    };

    struct Band {
        GeoZone::Restriction restriction = GeoZone::Unknown;
        QString name;
        QList<BandPoint> points;

        friend bool operator==(const Band &, const Band &) = default;
    };

    /// @return Bands of the zones along consecutive segments, see the bands property. Segments with a zero distance
    ///         and invalid coordinates only advance the distance.
    static QList<Band> computeBands(const QList<Segment> &segments, const GeoZoneManager *manager);
    /// @return The bands in the form of the bands property
    static QVariantList toVariantList(const QList<Band> &bands);
    static QVariantList bandsForSegments(const QList<Segment> &segments, const GeoZoneManager *manager) { return toVariantList(computeBands(segments, manager)); }

    /// Re-evaluates immediately instead of after changes settle
    Q_INVOKABLE void update();

signals:
    void missionControllerChanged();
    void bandsChanged();
    void activeChanged();

private:
    QList<Segment> _missionSegments() const;
    void _clear();

    GeoZoneManager *_manager = nullptr;
    QPointer<MissionController> _missionController;
    QList<Band> _bandList;
    QVariantList _bands;
    bool _active = true;
    QTimer _updateTimer;

    static constexpr int _updateDelayMSecs = 500;
};
