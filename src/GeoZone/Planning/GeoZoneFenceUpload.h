/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtPositioning/QGeoCircle>
#include <QtPositioning/QGeoCoordinate>

class GeoFenceController;
class GeoFenceManager;
class QGCFenceCircle;
class QGCFencePolygon;
class Vehicle;

/// Sends the fence of a plan to its vehicle like a plan upload does, then reads it back and compares it with the fence
/// sent. The upload goes through the states
///
///     Idle ──start()──> Sending ──sent──> Verifying ──read back──> Verified or Mismatch
///
/// and ends in Failed when the vehicle rejects the fence, the transfer fails or the vehicle disconnects, and in Aborted
/// when it is aborted. Each of the final states can start again.
class GeoZoneFenceUpload : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("GeoFenceController.h")

    Q_PROPERTY(GeoFenceController*  geoFenceController  READ geoFenceController WRITE setGeoFenceController NOTIFY geoFenceControllerChanged FINAL)
    Q_PROPERTY(State                state               READ state              NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString              message             READ message            NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool                 busy                READ busy               NOTIFY stateChanged FINAL)          ///< Sending or Verifying
    Q_PROPERTY(bool                 canUpload           READ canUpload          NOTIFY canUploadChanged FINAL)      ///< A vehicle supporting fences is connected

public:
    explicit GeoZoneFenceUpload(QObject *parent = nullptr);

    enum State {
        Idle,
        Sending,
        Verifying,      ///< Reading the fence back
        Verified,
        Mismatch,       ///< The fence read back differs from the one sent
        Failed,
        Aborted,        ///< See abort
    };
    Q_ENUM(State)

    static constexpr double verifyTolerance = 0.5;  ///< Meters vertices, circle centers and radii read back may differ by

    /// @return true if the upload may go from one state to the other
    static bool canTransition(State from, State to);

    GeoFenceController *geoFenceController() const { return _geoFenceController; }
    /// Aborts an upload in progress
    void setGeoFenceController(GeoFenceController *geoFenceController);

    State state() const { return _state; }
    QString message() const { return _message; }
    bool busy() const { return (_state == Sending) || (_state == Verifying); }
    bool canUpload() const;

    /// Sends the fence of the controller to the plan's vehicle, unless an upload is in progress
    Q_INVOKABLE void start();
    /// Stops following an upload in progress, which ends Aborted with the reason. The vehicle may still store the fence.
    Q_INVOKABLE void abort(const QString &reason);

    /// Fence sent to or read back from a vehicle
    struct FenceSnapshot {
        struct Polygon {
            bool inclusion = false;
            QList<QGeoCoordinate> vertices;

            bool operator==(const Polygon &other) const = default;
        };
        struct Circle {
            bool inclusion = false;
            QGeoCircle circle;

            bool operator==(const Circle &other) const = default;
        };
        QList<Polygon> polygons;
        QList<Circle> circles;
        QGeoCoordinate returnPoint;

        /// Exactly equal, in the same order
        bool operator==(const FenceSnapshot &other) const = default;
        /// @return Number of polygon vertices
        int pointCount() const;
    };
    static FenceSnapshot snapshot(GeoFenceController *geoFenceController);
    static FenceSnapshot snapshot(const QList<QGCFencePolygon> &polygons, const QList<QGCFenceCircle> &circles, const QGeoCoordinate &returnPoint);

    /// @return What differs between the fences beyond verifyTolerance, empty when they match. Fences may be in any order.
    static QString compareFences(const FenceSnapshot &sent, const FenceSnapshot &received);

signals:
    void geoFenceControllerChanged();
    void stateChanged();
    void canUploadChanged();

private:
    /// @return The plan's vehicle, null without controller
    Vehicle *_planVehicle() const;
    void _connectController();
    /// Moves to the state if the transition is allowed, disconnecting from the upload when it ends
    /// @return false if it isn't
    bool _transition(State state, const QString &message);
    void _sent(bool error);
    void _readBack();
    void _vehicleLost();
    QString _rejectionText(const QString &errorMessage) const;

    QPointer<GeoFenceController> _geoFenceController;
    QObject *_controllerConnections = nullptr;          ///< Receiver of the signals of the controller, deleted to disconnect them
    State _state = Idle;
    QString _message;

    // Of the upload in progress
    QObject *_uploadConnections = nullptr;              ///< Receiver of the signals of the upload, deleted to disconnect them
    QPointer<Vehicle> _vehicle;
    QPointer<GeoFenceManager> _manager;
    FenceSnapshot _sentFence;
    QString _error;                                     ///< Reported by the fence manager
    int _ackResult = 0;                                 ///< MAV_MISSION_RESULT of the last fence MISSION_ACK, 0 when accepted
};
