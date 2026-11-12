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
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtPositioning/QGeoCircle>
#include <QtPositioning/QGeoCoordinate>

#include <optional>

#include "GeoZoneFenceTags.h"
#include "GeoZoneFences.h"
#include "GeoZoneSourceVersion.h"

class GeoFenceController;
class GeoFenceManager;
class QGCFenceCircle;
class QGCFencePolygon;
class Vehicle;

/// Connections which are made and disconnected together, each by its handle, so that they can be disconnected from one
/// of their own slots and are gone at once: a receiver object can't be deleted from its own slot, and
/// QObject::disconnect() without a sender disconnects nothing
class GeoZoneFenceConnections
{
public:
    GeoZoneFenceConnections() = default;
    ~GeoZoneFenceConnections() { disconnect(); }

    Q_DISABLE_COPY_MOVE(GeoZoneFenceConnections)

    GeoZoneFenceConnections &operator<<(const QMetaObject::Connection &connection)
    {
        _connections.append(connection);
        return *this;
    }
    void disconnect()
    {
        for (const QMetaObject::Connection &connection : std::as_const(_connections)) {
            (void) QObject::disconnect(connection);
        }
        _connections.clear();
    }
    bool isEmpty() const { return _connections.isEmpty(); }

private:
    QList<QMetaObject::Connection> _connections;
};

/// Sends the fence of a plan to its vehicle like a plan upload does, then reads it back and compares it with the fence
/// sent. The upload goes through the states
///
///     Idle ──start()──> Sending ──sent──> Verifying ──read back──> Verified or Mismatch
///
/// and ends in Failed when the vehicle rejects the fence, the transfer fails or the vehicle disconnects, and in Aborted
/// when the plan's fence is edited or the plan file changes meanwhile, since the vehicle wouldn't store the plan's
/// fence. The plan's fence stays dirty unless the vehicle received it as it is. Each of the final states can
/// start again.
/// It also records the fence each vehicle stores, see Verification: the one an upload verified, or the one a plan upload
/// sent, with the zones its import left out, kept across restarts for the pre-flight checklist.
class GeoZoneFenceUpload : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("GeoFenceController.h")

    Q_PROPERTY(GeoFenceController*  geoFenceController  READ geoFenceController WRITE setGeoFenceController NOTIFY geoFenceControllerChanged FINAL)
    Q_PROPERTY(State                state               READ state              NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString              message             READ message            NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool                 busy                READ busy               NOTIFY stateChanged FINAL)          ///< Sending or Verifying
    Q_PROPERTY(bool                 canUpload           READ canUpload          NOTIFY canUploadChanged FINAL)      ///< A vehicle supporting fences is connected
    /// The upload failed because the vehicle has no room for the fence: it answered MAV_MISSION_NO_SPACE, or ArduPilot
    /// MAV_MISSION_ERROR, which it does for fences exceeding its storage. A smaller fence may be stored, see
    /// GeoZoneFenceImporter::retryWithFewerZones.
    Q_PROPERTY(bool                 rejectedForRoom     READ rejectedForRoom    NOTIFY stateChanged FINAL)

public:
    explicit GeoZoneFenceUpload(QObject *parent = nullptr);

    enum State {
        Idle,
        Sending,
        Verifying,      ///< Reading the fence back
        Verified,
        Mismatch,       ///< The fence read back differs from the one sent
        Failed,
        Aborted,        ///< The plan changed before the upload was verified, or see abort
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
    bool rejectedForRoom() const { return (_state == Failed) && _rejectedForRoom; }
    /// @return Storage the fence of the last upload takes on the vehicle, see rejectedForRoom
    GeoZoneFences::Usage sentUsage() const;

    /// Sends the fence of the controller to the plan's vehicle, unless an upload is in progress
    Q_INVOKABLE void start();
    /// Stops following an upload in progress, which ends Aborted with the reason. The vehicle may still store the fence.
    Q_INVOKABLE void abort(const QString &reason);
    /// Lets an upload in progress finish without its owner, like the dialog which started it and was closed: the upload
    /// then belongs to the plan's fence controller, is verified and recorded like any other, reports how it ended as an
    /// application message, see finishedDetached, and deletes itself.
    /// @return false if no upload is in progress, the upload then stays with its owner
    bool detach();

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

    /// Fence uploaded to a vehicle, kept until the next upload to it or until its fence is removed, also when the
    /// vehicle reconnects and across restarts, see setRecordsFile
    struct Verification {
        FenceSnapshot fence;
        QDateTime time;                         ///< UTC
        int geoZoneFences = 0;                  ///< Polygons and circles added from geozones, see GeoZoneFenceTag
        QList<GeoZoneSourceVersion> sources;    ///< Of the geozone data when it was uploaded
        QList<GeoZoneFenceTags::NotFenced> notFenced;   ///< Zones the import of the fences left out, see GeoZoneFenceTags::notFenced
        int notFencedCount = 0;                         ///< Also those not kept
        /// Read back from the vehicle and found to be the fence sent. False for the fence of a plan upload, which isn't
        /// read back.
        bool verified = true;

        QJsonObject toJson() const;
        static std::optional<Verification> fromJson(const QJsonObject &json);
    };
    /// @return The last fence uploaded to the vehicle, nothing without. Vehicles are told apart by the unique id they
    ///         report, or by their system id without one. The fence is the vehicle's while it compares equal to the one
    ///         the vehicle stores, see compareFences.
    static std::optional<Verification> verification(const Vehicle *vehicle);
    /// Records the fence uploaded to the vehicle, as uploads do
    static void setVerification(Vehicle *vehicle, const Verification &verification);
    /// Forgets the fence recorded for the vehicle, like when its fence is removed
    static void clearVerification(Vehicle *vehicle);

    static constexpr int maxRecordedVehicles = 16;  ///< Vehicles whose fences are kept, the most recent ones
    /// Keeps the recorded fences in the file instead of fences/vehicle-fences.json of the geozone storage, forgetting
    /// those in memory, so that they are read from the file again like after a restart. Empty for the default.
    static void setRecordsFile(const QString &fileName);

signals:
    void geoFenceControllerChanged();
    void stateChanged();
    void canUploadChanged();
    /// A detached upload ended in the state with the message, which the application shows
    void finishedDetached(GeoZoneFenceUpload::State state, const QString &message);

private:
    /// @return The plan's vehicle, null without controller
    Vehicle *_planVehicle() const;
    void _connectController();
    /// Moves to the state if the transition is allowed, disconnecting from the upload when it ends
    /// @return false if it isn't
    bool _transition(State state, const QString &message);
    void _sent(bool error);
    void _readBack();
    /// Follows the fence transfers of the plan's vehicle, see _planFenceSent
    void _connectPlanVehicle();
    /// Records the fence a plan upload sent to the vehicle when it is the fence of the controller, not verified
    void _planFenceSent(Vehicle *vehicle, bool error);
    /// @return Record of the fence uploaded from the plan of the controller, with its zones not fenced
    Verification _record(const FenceSnapshot &fence) const;
    /// Aborts the upload in progress when the plan's fence no longer is the one sent
    void _checkFence();
    void _vehicleLost();
    QString _rejectionText(const QString &errorMessage) const;

    QPointer<GeoFenceController> _geoFenceController;
    QObject *_controllerConnections = nullptr;          ///< Receiver of the signals of the controller, deleted to disconnect them
    QObject *_planVehicleConnections = nullptr;         ///< Receiver of the signals of the plan's vehicle, deleted to disconnect them
    State _state = Idle;
    QString _message;

    // Of the upload in progress
    GeoZoneFenceConnections _uploadConnections;         ///< Of the signals of the upload, disconnected when it ends
    quint64 _uploadNumber = 0;                          ///< Counts the uploads started, for what an upload posted to itself
    QPointer<Vehicle> _vehicle;
    QPointer<GeoFenceManager> _manager;
    QString _planFile;                                  ///< Of the plan when the upload started
    FenceSnapshot _sentFence;
    Verification _sentRecord;                           ///< Of the fence sent, with the zones the plan didn't fence then
    QString _error;                                     ///< Reported by the fence manager
    int _ackResult = 0;                                 ///< MAV_MISSION_RESULT of the last fence MISSION_ACK, 0 when accepted
    bool _rejectedForRoom = false;
};
