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
class GeoZoneMissionWorker;
class MissionController;
class VisualMissionItem;

/// Suggests how the legs of a mission between waypoints can avoid the restricted zones they enter, see
/// GeoZoneRouteSearch: a detour around the zones, which can be inserted as waypoints, or climbing over or descending
/// below the zones, which can raise or lower the waypoints of the leg. The cheapest way is recommended, the others are
/// listed with why the ways that don't work don't.
/// The routes are searched on a worker thread with a snapshot of the zones, except for update, while the dialog
/// shows the progress and can cancel it. A new computation cancels the one in progress.
/// Suggestions refer to the items of the mission they were searched for: a change of the mission or of the zones drops
/// them at once, with the search in progress, and they are searched again once the changes settle. A suggestion is
/// only applied to the items it was made for.
class GeoZoneRoutePlanner : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("MissionController.h")

    Q_PROPERTY(MissionController*   missionController   READ missionController  WRITE setMissionController  NOTIFY missionControllerChanged FINAL)
    /// Maps with text, canApply when the suggestion can change the mission and applyText for the button doing it, kind
    /// ("detour", "climb", "descent", or "none" for legs without a way around the zones), recommended for the cheapest
    /// way of each leg, legText naming the leg, first for the first suggestion of each leg, and notes, on the last one,
    /// explaining why the other ways don't work, with suggestedMaxAltitude, the meters above the terrain a climb blocked
    /// by maxAltitude needs (absent without), which can be set as maxAltitude
    Q_PROPERTY(QVariantList         suggestions         READ suggestions                                    NOTIFY suggestionsChanged FINAL)
    /// Suggestions are computed and follow mission and zone changes while active, like while they are shown
    Q_PROPERTY(bool                 active              READ active             WRITE setActive             NOTIFY activeChanged FINAL)
    Q_PROPERTY(double               margin              READ margin                                         CONSTANT FINAL)       ///< Meters, see detourMargin
    Q_PROPERTY(double               verticalMargin      READ verticalMargin                                 CONSTANT FINAL)       ///< Meters, see floorMargin
    /// Meters above the terrain that climbs over zones may reach
    Q_PROPERTY(double               maxAltitude         READ maxAltitude        WRITE setMaxAltitude        NOTIFY maxAltitudeChanged FINAL)
    /// maxAltitude in whole vertical distance units of the application, for the field which edits it with setMaxAltitudeText
    Q_PROPERTY(QString              maxAltitudeText     READ maxAltitudeText                                NOTIFY maxAltitudeChanged FINAL)
    /// The suggestions are being computed, or will be once the changes of the mission or the zones settle
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
    bool active() const;
    void setActive(bool active);
    double margin() const { return detourMargin; }
    double verticalMargin() const { return floorMargin; }
    double maxAltitude() const { return _maxAltitude; }
    void setMaxAltitude(double maxAltitude);
    QString maxAltitudeText() const;
    /// Sets maxAltitude from the text of the field, in the vertical distance units of the application, when it is a
    /// number of at least 1 and not the text shown: that text is rounded, so that a field giving it back when it
    /// loses the focus would change the limit, like to just below what a climb needs
    Q_INVOKABLE void setMaxAltitudeText(const QString &text);
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

    /// Inserts the waypoints of a detour into the mission, at the altitudes the detour was checked at, or changes the
    /// altitudes of the waypoints of a leg. The suggestions are then dropped, and searched again once the mission
    /// settles while active.
    /// @return true if the mission changed. false when there is nothing to apply, or, with applyFailed telling why,
    ///         when the mission isn't the one the suggestion was made for or the altitudes can't be set: the mission
    ///         is then left as it was.
    Q_INVOKABLE bool applySuggestion(int index);

    /// @return The altitude an item in the altitude mode (QGroundControlQmlGlobal::AltMode) has at the altitude above
    ///         mean sea level: above home, above the terrain at the item or above mean sea level. Nothing when what
    ///         the mode refers to isn't known, and in the modes which aren't those of items.
    static std::optional<double> altitudeInMode(int altitudeMode, double altitudeAMSL, double homeAltitudeAMSL, double terrainAMSL);

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
    /// A suggestion couldn't be applied, the mission is as it was
    void applyFailed(const QString &message);

private:
    class Terrain;

    /// Leg of the mission to search routes for
    struct LegJob {
        GeoZoneRouteSearch::Leg leg;
        int fromItemIndex = -1;
        int toItemIndex = -1;
        QPointer<QObject> fromItem;     ///< The visual items at the indexes when the job was made, only used on the GUI thread
        QPointer<QObject> toItem;
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

    /// New altitude of an item of the mission
    struct ItemAltitude {
        int itemIndex = -1;
        QPointer<QObject> item;     ///< The visual item at the index when the suggestion was made
        int altitudeMode = -1;      ///< Of the item then, which the altitude is in
        double altitude = 0;
    };

    struct Suggestion {
        QString text;
        QString kind;
        QString legText;
        QStringList notes;
        bool recommended = false;
        bool first = false;
        int insertIndex = -1;               ///< Visual item index where the detour waypoints go, -1 without a detour
        QPointer<QObject> insertBefore;     ///< The visual item at insertIndex when the suggestion was made, the end of the leg
        QList<QGeoCoordinate> waypoints;
        /// Meters AMSL the detour was checked at, changing evenly along it, NaN for the detour of a leg at unknown
        /// altitudes, which was checked at all altitudes
        QList<double> waypointAltitudes;
        QList<ItemAltitude> altitudes;      ///< Climbs and descents: the items of the leg with their new altitudes
        double suggestedMaxAltitude = qQNaN();      ///< Last of a leg: meters above the terrain a climb needs beyond maxAltitude, NaN without
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
    /// Cancels the computation in progress or scheduled, whose results are then ignored
    void _stop();
    /// Drops the suggestions and the computation in progress, which are of the mission or the zones as they were, and
    /// schedules another one
    void _invalidate();
    /// @return The visual item at the index of the mission when it is the expected one, null when the mission changed
    VisualMissionItem *_itemAt(int index, const QPointer<QObject> &expected) const;
    /// Inserts the waypoints of the detour at the altitudes it was checked at, or none of them
    /// @return false if their altitudes can't be set
    bool _insertDetour(const Suggestion &suggestion);
    void _clearSuggestions();
    void _setBusy(bool busy);
    void _setProgress(double progress);
    void _setStatusText(const QString &statusText);

    /// @return New altitudes of the leg's items in their altitude modes, empty when they can't be changed
    QList<ItemAltitude> _itemAltitudes(const LegJob &legJob, const GeoZoneRouteSearch::Route &route) const;
    QString _routeText(const QString &legText, const LegJob &legJob, const GeoZoneRouteSearch::Route &route) const;
    QString _failureText(const LegJob &legJob, const GeoZoneRouteSearch::Route &route) const;
    /// @param roundUp Like raised altitudes, down like lowered ones
    QString _altitudeText(const LegJob &legJob, double altitudeAMSL, bool roundUp) const;
    static QString _distanceText(double meters);
    static QString _heightText(double meters);

    GeoZoneManager *_manager = nullptr;
    GeoZoneMissionWorker *_worker = nullptr;    ///< Follows the mission, searches the routes and drops outdated results
    QList<Suggestion> _suggestions;
    double _maxAltitude = defaultMaxAltitude;
    bool _busy = false;
    double _progress = 0;
    QString _statusText;
    std::shared_ptr<Terrain> _terrain;      ///< Ground elevations the searches looked up, kept between computations

    static constexpr int _updateDelayMSecs = 500;
};
