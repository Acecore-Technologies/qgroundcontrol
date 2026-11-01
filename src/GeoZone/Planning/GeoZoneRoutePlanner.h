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

#include <functional>
#include <memory>
#include <optional>

#include "GeoZone.h"
#include "GeoZoneCancelToken.h"
#include "GeoZoneCatalog.h"
#include "GeoZonePolicy.h"
#include "GeoZoneRouteSearch.h"

class GeoZoneAuthorisations;
class GeoZoneManager;
class MissionController;

/// Suggests how the legs of a mission between waypoints can avoid the restricted zones they enter, see
/// GeoZoneRouteSearch: a detour around the zones, which can be inserted as waypoints, or climbing over or descending
/// below the zones, which can raise or lower the waypoints of the leg. The cheapest way is recommended, the others are
/// listed with why the ways that don't work don't.
/// The routes are searched on a worker thread with a snapshot of the zones, except for update, while the dialog
/// shows the progress and can cancel it. A new computation cancels the one in progress.
class GeoZoneRoutePlanner : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("MissionController.h")

    Q_PROPERTY(MissionController*   missionController   READ missionController  WRITE setMissionController  NOTIFY missionControllerChanged FINAL)
    /// Maps with text, canApply when the suggestion can change the mission and applyText for the button doing it, kind
    /// ("detour", "climb", "descent", or "none" for legs without a way around the zones), recommended for the cheapest
    /// way of each leg, legText naming the leg, first for the first suggestion of each leg, and notes, on the last one,
    /// explaining why the other ways don't work
    Q_PROPERTY(QVariantList         suggestions         READ suggestions                                    NOTIFY suggestionsChanged FINAL)
    /// Suggestions are computed and follow mission and zone changes while active, like while they are shown
    Q_PROPERTY(bool                 active              READ active             WRITE setActive             NOTIFY activeChanged FINAL)
    Q_PROPERTY(double               margin              READ margin                                         CONSTANT FINAL)       ///< Meters, see detourMargin
    Q_PROPERTY(double               verticalMargin      READ verticalMargin                                 CONSTANT FINAL)       ///< Meters, see floorMargin
    /// Meters above the terrain that climbs over zones may reach
    Q_PROPERTY(double               maxAltitude         READ maxAltitude        WRITE setMaxAltitude        NOTIFY maxAltitudeChanged FINAL)
    /// The suggestions are being computed
    Q_PROPERTY(bool                 busy                READ busy                                           NOTIFY busyChanged FINAL)
    Q_PROPERTY(double               progress            READ progress                                       NOTIFY progressChanged FINAL)    ///< From 0 to 1 while busy
    /// Why there are no suggestions though legs may enter zones, like a cancelled computation, empty otherwise
    Q_PROPERTY(QString              statusText          READ statusText                                     NOTIFY statusTextChanged FINAL)

public:
    explicit GeoZoneRoutePlanner(QObject *parent = nullptr);
    /// @param manager Zones to avoid, GeoZoneManager::instance() when null
    explicit GeoZoneRoutePlanner(GeoZoneManager *manager, QObject *parent = nullptr);
    ~GeoZoneRoutePlanner() override;

    static constexpr double detourMargin = 100;         ///< Meters kept from the zones horizontally
    static constexpr double floorMargin = 10;           ///< Meters kept below floors and above ceilings
    static constexpr double minGroundClearance = 10;    ///< Meters above the terrain that lowered legs keep
    static constexpr double defaultMaxAltitude = 120;   ///< Meters above the terrain that raised legs may reach, see maxAltitude
    static constexpr int maxTerrainRounds = 2;          ///< Searches again with the terrain their detours needed

    MissionController *missionController() const;
    void setMissionController(MissionController *missionController);
    QVariantList suggestions() const;
    bool active() const { return _active; }
    void setActive(bool active);
    double margin() const { return detourMargin; }
    double verticalMargin() const { return floorMargin; }
    double maxAltitude() const { return _maxAltitude; }
    void setMaxAltitude(double maxAltitude);
    bool busy() const { return _busy; }
    double progress() const { return _progress; }
    QString statusText() const { return _statusText; }

    /// @return Settings of the searches, see GeoZoneRouteSearch
    GeoZoneRouteSearch::Settings settings() const;

    /// Computes the suggestions for the current mission on the calling thread, cancelling a computation in progress
    Q_INVOKABLE void update();
    /// Starts computing the suggestions for the current mission on a worker thread, cancelling one in progress
    Q_INVOKABLE void startUpdate();
    /// Stops the computation in progress, leaving no suggestions
    Q_INVOKABLE void cancel();

    /// Inserts the waypoints of a detour into the mission or changes the altitudes of the waypoints of a leg, then
    /// computes the suggestions again
    /// @return true if the mission changed
    Q_INVOKABLE bool applySuggestion(int index);

    /// @return Waypoints between from and to which keep the margin from all volumes of the zones horizontally, nothing
    ///         if an end is inside them or they surround it. Empty when the straight leg already keeps the margin.
    static std::optional<QList<QGeoCoordinate>> detourAround(const QGeoCoordinate &from, const QGeoCoordinate &to, const QList<const GeoZone*> &zones,
                                                             double margin = detourMargin);

signals:
    void missionControllerChanged();
    void suggestionsChanged();
    void activeChanged();
    void maxAltitudeChanged();
    void busyChanged();
    void progressChanged();
    void statusTextChanged();

private:
    class Terrain;

    /// Leg of the mission to search routes for
    struct LegJob {
        GeoZoneRouteSearch::Leg leg;
        int fromItemIndex = -1;
        int toItemIndex = -1;
        int fromSequenceNumber = -1;
        int toSequenceNumber = -1;
    };

    /// What a computation needs, copied on the GUI thread so that it can run on a worker thread
    struct Job {
        GeoZoneCatalog catalog;
        GeoZonePolicy policy;
        std::shared_ptr<const GeoZoneAuthorisations> authorisations;    ///< Copy which the policy refers to
        QDateTime time;
        GeoZoneRouteSearch::Settings settings;
        QList<LegJob> legs;
        std::shared_ptr<Terrain> terrain;
    };

    struct LegResult {
        int leg = -1;                       ///< Index into Job::legs
        GeoZoneRouteSearch::Result result;
        bool terrainMissing = false;        ///< The search needed terrain which the job didn't have
    };

    struct Suggestion {
        QString text;
        QString kind;
        QString legText;
        QStringList notes;
        bool recommended = false;
        bool first = false;
        int insertIndex = -1;               ///< Visual item index where the detour waypoints go, -1 without a detour
        QList<QGeoCoordinate> waypoints;
        QList<double> waypointAltitudes;    ///< Meters AMSL of the waypoints, changing evenly along the detour, NaN when unknown
        QList<std::pair<int, double>> altitudes;    ///< Visual item indexes with their new altitude in the item's altitude mode
    };

    Job _makeJob() const;
    /// @return Routes of the legs at the indexes, searched on the calling thread
    static QList<LegResult> _run(const Job &job, const QList<int> &legs, const GeoZoneCancelToken &cancel, const std::function<void(int done)> &progress);
    /// Searches the legs on a worker thread, then those which missed terrain again or applies the results
    void _launch(const std::shared_ptr<const Job> &job, const QList<int> &legs, const QList<LegResult> &results, int round);
    /// @return The legs whose search missed terrain which is known now, empty when none or after the last round
    static QList<int> _legsToRetry(const Job &job, const QList<LegResult> &results, int round);
    static void _merge(QList<LegResult> &results, const QList<LegResult> &update);
    void _apply(const Job &job, const QList<LegResult> &results);
    /// Cancels the computation in progress, whose results are then ignored
    void _stop();
    void _clearSuggestions();
    void _setBusy(bool busy);
    void _setProgress(double progress);
    void _setStatusText(const QString &statusText);

    /// @return New altitudes of the leg's items in their altitude modes, empty when they can't be changed
    QList<std::pair<int, double>> _itemAltitudes(const LegJob &legJob, const GeoZoneRouteSearch::Route &route) const;
    QString _routeText(const QString &legText, const LegJob &legJob, const GeoZoneRouteSearch::Route &route) const;
    QString _failureText(const LegJob &legJob, const GeoZoneRouteSearch::Route &route) const;
    /// @param roundUp Like raised altitudes, down like lowered ones
    QString _altitudeText(const LegJob &legJob, double altitudeAMSL, bool roundUp) const;
    static QString _distanceText(double meters);
    static QString _heightText(double meters);

    GeoZoneManager *_manager = nullptr;
    QPointer<MissionController> _missionController;
    QList<Suggestion> _suggestions;
    bool _active = false;
    double _maxAltitude = defaultMaxAltitude;
    bool _busy = false;
    double _progress = 0;
    QString _statusText;
    QTimer _updateTimer;
    quint64 _generation = 0;                ///< Of the computation whose results are wanted
    GeoZoneCancelToken _cancel;             ///< Of the computation in progress
    std::shared_ptr<Terrain> _terrain;      ///< Ground elevations the searches looked up, kept between computations
};
