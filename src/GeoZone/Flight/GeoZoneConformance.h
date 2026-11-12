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
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>

#include "GeoZone.h"
#include "GeoZoneMonitor.h"
#include "GeoZoneOperationalIntent.h"

class GeoZoneManager;
class PlanMasterController;
class Vehicle;

/// Conformance monitoring of the active vehicle to the operational intent of its mission, like a USS does in U-space:
/// while the planned flight has a start, see GeoZonePlanning::plannedStart, the 4D volumes of the vehicle's mission
/// from the planned start are built like the export does, see GeoZoneOperationalIntent::buildVolumes, and the flying
/// vehicle is checked against the volumes whose time window contains the current time. When it leaves them
/// horizontally or vertically, or no volume applies at that time, the monitor alerts "Outside planned volume" once,
/// through the usual alert path, until the vehicle conforms again. Without planned start or mission it is off.
class GeoZoneConformance : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("PlanMasterController.h")

    Q_PROPERTY(State    state       READ state      NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool     active      READ active     NOTIFY stateChanged FINAL)     ///< Volumes are monitored
    Q_PROPERTY(bool     conforming  READ conforming NOTIFY stateChanged FINAL)     ///< Not outside the volumes
    Q_PROPERTY(QString  stateText   READ stateText  NOTIFY stateChanged FINAL)

public:
    /// Checks the active vehicle of MultiVehicleManager every GeoZoneMonitor::evaluateIntervalMSecs while there are
    /// volumes, alerting through GeoZoneMonitor::instance(), against the mission of the Fly View, see followFlyViewPlan
    explicit GeoZoneConformance(QObject *parent = nullptr);
    /// Checks the positions passed to evaluate(), alerting through the monitor
    GeoZoneConformance(GeoZoneManager *manager, GeoZoneMonitor *monitor, QObject *parent = nullptr);
    ~GeoZoneConformance() override;

    static GeoZoneConformance *instance();
    /// Plan of the Fly View, whose mission the active vehicle flies, see GeoZonePlanData::follow. The application's
    /// instance follows it, also when it is created later.
    static void followFlyViewPlan(PlanMasterController *planController);

    enum State {
        Off,                    ///< No operational intent: no planned start or mission
        Waiting,                ///< The vehicle doesn't fly
        Conforming,
        OutsideHorizontally,    ///< Beside the volumes of the current time
        Above,
        Below,
        OutsideTime,            ///< No volume applies at the current time
    };
    Q_ENUM(State)

    /// How a position conforms to the volumes
    struct Result {
        State state = Off;
        double distance = qQNaN();  ///< Meters outside the nearest volume of the current time, NaN when unknown
    };

    using Volume = GeoZoneOperationalIntent::Volume;

    State state() const { return _result.state; }
    const Result &result() const { return _result; }
    bool active() const { return _result.state != Off; }
    bool conforming() const { return (_result.state == Conforming) || (_result.state == Waiting); }
    QString stateText() const { return stateText(_result); }

    /// Plan whose mission the active vehicle flies, the Fly View's
    void setPlanController(PlanMasterController *planController);
    PlanMasterController *planController() const;
    /// Monitors these volumes rather than those of the mission, like tests do
    void setVolumes(const QList<Volume> &volumes);
    const QList<Volume> &volumes() const { return _volumes; }
    /// Builds the volumes of the mission and planned start again
    void updateVolumes();

    /// Checks the position of the vehicle at the current time, alerting when it leaves the volumes while it flies
    void evaluate(const GeoZone::Position &position, bool flying, int vehicleId = GeoZoneMonitor::noVehicle);

    /// @return How the position conforms at the time to the volumes, Off without them, see the class description. A
    ///         position whose altitude isn't known, or can't be compared with the volumes for lack of the height of the
    ///         geoid, conforms vertically.
    static Result check(const QList<Volume> &volumes, const GeoZone::Position &position, const QDateTime &time);
    /// @param geoidHeight Meters of the geoid above the ellipsoid at the position, NaN when unknown, see
    ///                    GeoZoneGeometry::geoidHeight
    static Result check(const QList<Volume> &volumes, const GeoZone::Position &position, const QDateTime &time, double geoidHeight);
    /// @return Meters from the coordinate to the outline of the volume, 0 inside
    static double horizontalDistance(const Volume &volume, const QGeoCoordinate &coordinate);
    /// @return Like "Outside planned volume: 40 m above it", or "Inside planned volume"
    static QString stateText(const Result &result);
    /// @return Code of the log's zone identifier of the alerts in the state, see GeoZoneMonitor::AlertEvent::NonConformance
    static QString stateCode(State state);

signals:
    void stateChanged();
    void volumesChanged();

private:
    void _connectPlan();
    void _scheduleVolumes();
    void _evaluateActiveVehicle();
    void _setResult(const Result &result);
    void _updateEvaluateTimer();

    GeoZoneManager *_manager = nullptr;
    QPointer<GeoZoneMonitor> _monitor;
    QPointer<PlanMasterController> _planController;
    QObject *_planConnections = nullptr;    ///< Receiver of the signals of the plan, replaced to disconnect them
    QList<Volume> _volumes;
    bool _fixedVolumes = false;             ///< Set by setVolumes()
    Result _result;
    QTimer _volumesTimer;                   ///< Changes of the mission settle before the volumes are built
    QTimer _evaluateTimer;                  ///< Of the application's instance
    bool _application = false;
};
