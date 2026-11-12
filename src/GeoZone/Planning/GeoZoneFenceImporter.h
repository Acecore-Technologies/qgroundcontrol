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
#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtPositioning/QGeoCircle>
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoRectangle>

#include <optional>
#include <utility>

#include "GeoFenceController.h"
#include "GeoZoneBox.h"
#include "GeoZoneCatalog.h"
#include "GeoZoneFenceChoiceModel.h"
#include "GeoZoneFenceTags.h"
#include "GeoZoneFenceUpload.h"
#include "GeoZoneFences.h"
#include "GeoZonePolicy.h"

class GeoZone;
class GeoZoneMissionWorker;
class Vehicle;

/// Adds loaded geozones to a GeoFence as exclusion fences. Polygon holes are dropped, so the whole outer
/// boundary is excluded, and fences identical to existing ones are not added again.
/// Selections fit zones into the fence storage of the plan's vehicle, the zones most relevant for the planned flight
/// first, see GeoZoneFences::select, and replace the fences from geozones. The fence can then be uploaded and read
/// back from the vehicle to verify it, see GeoZoneFenceUpload.
/// The zones of a selection are prepared once, see GeoZoneFences::prepare, on the thread pool for updateSelection,
/// so that including or leaving out zones selects again quickly.
class GeoZoneFenceImporter : public QObject
{
    Q_OBJECT

    Q_PROPERTY(GeoFenceController*  geoFenceController  READ geoFenceController WRITE setGeoFenceController NOTIFY geoFenceControllerChanged FINAL)
    Q_PROPERTY(QVariantMap              capacity        READ capacityInfo       NOTIFY selectionChanged FINAL)  ///< See capacityInfo
    Q_PROPERTY(GeoZoneFenceChoiceModel* choices         READ choices            CONSTANT FINAL)                 ///< The selected zones and all those left out, in priority order
    Q_PROPERTY(int                      selectedCount   READ selectedCount      NOTIFY selectionChanged FINAL)
    Q_PROPERTY(int                      leftOutCount    READ leftOutCount       NOTIFY selectionChanged FINAL)  ///< All zones left out, also those not listed
    /// Zones the selection doesn't fence, see GeoZoneFences::Choice::fenced: those left out except the duplicates of
    /// selected zones, which their fences cover
    Q_PROPERTY(int                      notFencedCount  READ notFencedCount     NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString                  notFencedText   READ notFencedText      NOTIFY selectionChanged FINAL)  ///< Of the selection, see GeoZoneFenceTags::notFencedText
    /// The zones the plan's last import left out, like "3 geozones are not fenced: A (...), ...", while its fences from
    /// geozones are unchanged, see GeoZoneFenceTags::notFenced. Empty otherwise.
    Q_PROPERTY(QString                  fenceNote       READ fenceNote          NOTIFY fenceNoteChanged FINAL)
    Q_PROPERTY(int                      fenceNoteCount  READ fenceNoteCount     NOTIFY fenceNoteChanged FINAL)  ///< Zones of the fence note
    Q_PROPERTY(QStringList              fenceNoteZones  READ fenceNoteZones     NOTIFY fenceNoteChanged FINAL)  ///< Each zone of the fence note and why, all of them
    /// The zones the fence on the plan's vehicle leaves out, as recorded when it was uploaded, while the vehicle stores
    /// that fence, see GeoZoneFenceUpload::verification: also after a restart or a reconnect, and with a plan which
    /// doesn't have them. Like "On the vehicle: 3 geozones are not fenced: ...", empty otherwise.
    Q_PROPERTY(QString                  vehicleFenceNote        READ vehicleFenceNote       NOTIFY vehicleFenceNoteChanged FINAL)
    Q_PROPERTY(QStringList              vehicleFenceNoteZones   READ vehicleFenceNoteZones  NOTIFY vehicleFenceNoteChanged FINAL)   ///< Each of them and why
    Q_PROPERTY(bool                     busy            READ busy               NOTIFY busyChanged FINAL)       ///< While zones are prepared for updateSelection
    Q_PROPERTY(GeoZoneFenceUpload*      upload          READ upload             CONSTANT FINAL)                 ///< Of the plan's fence to its vehicle
    Q_PROPERTY(QString                  staleText       READ staleText          NOTIFY staleChanged FINAL)      ///< Why the plan's fences from geozones are outdated, empty when they aren't, see GeoZoneFenceTags::staleZones

public:
    explicit GeoZoneFenceImporter(QObject *parent = nullptr);

    /// Special values of the activeWithinHours of updateSelection
    enum ActiveTime {
        Now = 0,            ///< Active from now for nowFlightSecs, or to the estimated end of the mission when that is later
        AnyTime = -1,       ///< Whatever the zone schedules, also zones which are inactive: only when the user asks for it
        AtFlightTime = -2,  ///< Active from the planned start to the estimated end, see GeoZonePlanning, or Now without planned start
    };
    Q_ENUM(ActiveTime)

    static constexpr int nowFlightSecs = 3600;      ///< Shortest time from now the zones fenced with Now must be active for
    static constexpr int maxActiveHours = 24 * 366; ///< Largest activeWithinHours, the others select like Now
    static constexpr double maxSimplifyTolerance = 500;     ///< Meters, the largest tolerance fences are simplified by

    /// @return The exclusion polygons and circles of the GeoFence, see GeoZoneFences::fencesCover
    static GeoZoneFences::ZoneFences exclusionFences(GeoFenceController *geoFenceController);

    /// @return What the fence of the vehicle can store: PX4's dataman for its firmware version, ArduPilot's fence
    ///         storage on the SD card, or estimated for the vehicle type and the storage of its flight controller, a
    ///         conservative estimate without a vehicle, nothing without fence support. Less after the vehicle rejected
    ///         a fence for lack of room, see reduceCapacity.
    static GeoZoneFences::Capacity vehicleCapacity(Vehicle *vehicle);
    /// Lowers the capacity assumed for the vehicle, until it disconnects, after it rejected a fence with the usage for
    /// lack of room, see GeoZoneFences::Capacity::reducedBelow
    static void reduceCapacity(Vehicle *vehicle, const GeoZoneFences::Usage &rejected);

    /// @return Storage of the plan fences selections keep: those not from geozones and the breach return point
    static GeoZoneFences::Usage keptUsage(GeoFenceController *geoFenceController);

    GeoFenceController *geoFenceController() const { return _geoFenceController; }
    void setGeoFenceController(GeoFenceController *geoFenceController);

    /// Adds exclusion fences for the zones
    /// @return Number of fence polygons and circles added
    int addExclusions(const QList<const GeoZone*> &zones, double simplifyTolerance = 0);
    /// Same as addExclusions, for GeoZoneInfo values from QML
    Q_INVOKABLE int addZones(const QVariantList &zones, double simplifyTolerance = 0);

    /// Selects zones of GeoZoneManager like select, preparing them on the thread pool: the selection changes once they
    /// are prepared, unless another selection starts meanwhile
    ///     @param restrictions: GeoZone::Restriction values of the zones. Other values select the zones of unknown
    ///                          restriction.
    ///     @param area: Only zones intersecting this area. Invalid for zones anywhere.
    ///     @param activeWithinHours: Only zones active at some time in the next hours, or see ActiveTime. Except for
    ///                               AnyTime, the zones active only part of that time aren't fenced, see
    ///                               GeoZoneFences::Candidate::partTime, since the vehicle's fence has no schedule.
    ///     @param maxSimplifyTolerance: Meters fences may be simplified by to fit, see GeoZoneFences::zoneFences, up
    ///                                  to maxSimplifyTolerance. Not simplified for a negative tolerance or one which
    ///                                  isn't a number.
    Q_INVOKABLE void updateSelection(const QVariantList &restrictions, const QGeoRectangle &area, int activeWithinHours = Now, double maxSimplifyTolerance = 0);

    /// Selects which of the zones fit into the fence of the plan's vehicle besides the fences kept, the zones entered by or
    /// near the planned flight first, see GeoZoneFences::select. Simplification steps up to the tolerance when needed.
    void select(const QList<const GeoZone*> &zones, double maxSimplifyTolerance = 0);
    const GeoZoneFences::Selection &selection() const { return _selection; }
    const GeoZoneFences::Capacity &capacity() const { return _capacity; }
    int selectedCount() const { return _selectedCount; }
    int leftOutCount() const { return static_cast<int>(_selection.choices.count()) - _selectedCount; }
    int notFencedCount() const { return static_cast<int>(_notFenced.count()); }
    QString notFencedText() const;
    /// @return The zones of the selection which aren't fenced, see notFencedCount, with why
    const QList<GeoZoneFenceTags::NotFenced> &notFenced() const { return _notFenced; }
    QString fenceNote() const { return _fenceNote; }
    int fenceNoteCount() const { return _fenceNoteCount; }
    QStringList fenceNoteZones() const { return _fenceNoteZones; }
    QString vehicleFenceNote() const { return _vehicleFenceNote; }
    QStringList vehicleFenceNoteZones() const { return _vehicleFenceNoteZones; }
    bool busy() const { return _busy; }
    GeoZoneFenceChoiceModel *choices() const { return _choices; }

    /// Includes or leaves out the zone with the key, see GeoZone::key, and selects again
    Q_INVOKABLE void setZoneIncluded(const QString &key, bool included);

    /// Replaces the fences from geozones in the plan with those of the selected zones
    /// @return Number of fence polygons and circles added
    Q_INVOKABLE int applySelection();

    /// Replaces the fences from geozones in the plan with a selection made with the settings they were last selected
    /// with, see GeoZoneFenceTags::importSettings, or, for fences without them, of their zones still loaded,
    /// simplified up to rebuildSimplifyTolerance
    /// @return Number of fence polygons and circles added
    Q_INVOKABLE int rebuild();
    static constexpr double rebuildSimplifyTolerance = 50;  ///< Meters

    QString staleText() const { return _staleText; }

    /// Applies the selection and uploads the plan's fence to the vehicle, see GeoZoneFenceUpload, without asking about
    /// the zones it doesn't fence: for callers which confirmed them, see requestUpload
    void uploadToVehicle();
    GeoZoneFenceUpload *upload() const { return _upload; }

    /// Uploads the selection once it is settled, so that what is asked about and uploaded is the selection of the last
    /// change, also while its zones are still being prepared. A selection which fences all its zones is uploaded, see
    /// uploadToVehicle. Otherwise nothing is uploaded and uploadConfirmationRequired names the zones it doesn't fence.
    Q_INVOKABLE void requestUpload();
    /// Uploads the selection whose zones not fenced were confirmed, see uploadConfirmationRequired. A selection which
    /// changed since, so that other zones aren't fenced, is asked about again instead.
    Q_INVOKABLE void confirmUpload(const QString &confirmation);
    /// After the vehicle rejected the fence for lack of room, see GeoZoneFenceUpload::rejectedForRoom: assumes the next
    /// smaller fence storage for it, see reduceCapacity, selects the zones which fit into it and asks to upload them,
    /// see requestUpload
    Q_INVOKABLE void retryWithFewerZones();

    /// @return Map with text ("Vehicle fence: 40 of 64 points", or "40 of about 37 points (estimated)" for limits the
    ///         vehicle may not have), note (where the limits come from), warning (fence parameters which keep the
    ///         vehicle from enforcing exclusion fences), usedItems, maxItems, usedBytes, storageBytes, keptItems,
    ///         circles, supported, estimated and reduced
    QVariantMap capacityInfo() const;

signals:
    void geoFenceControllerChanged();
    void selectionChanged();
    void busyChanged();
    void staleChanged();
    void fenceNoteChanged();
    void vehicleFenceNoteChanged();
    /// The settled selection doesn't fence zones, which the user must confirm before it is uploaded
    /// @param confirmation Identifies the zones, for confirmUpload
    /// @param text See notFencedText
    /// @param zones Each of the zones and why
    void uploadConfirmationRequired(const QString &confirmation, const QString &text, const QStringList &zones);

private:
    /// Fences for the zones which don't already exist in the fence, with their zones
    struct Fences {
        QList<QList<QGeoCoordinate>> polygons;
        QList<const GeoZone*> polygonZones;
        QList<QGeoCircle> circles;
        QList<const GeoZone*> circleZones;
    };

    /// Zones of the catalog in an area, for updateSelection
    struct AreaQuery {
        GeoZoneBox area;
        GeoZonePolicy policy;
        QDateTime activeFrom;   ///< Zones not active during the whole time from activeFrom to activeTo are left out,
        QDateTime activeTo;     ///< see GeoZoneFences::Candidate::partTime. Invalid for any time.
    };

    /// @return When the zones fenced must be active from and to, both invalid for AnyTime
    std::pair<QDateTime, QDateTime> _activeTimes(int activeWithinHours) const;

    /// What preparing a selection needs, gathered on the GUI thread
    struct PrepareInput {
        GeoZoneCatalog catalog;                     ///< Keeps its zones while they are prepared
        std::optional<AreaQuery> query;             ///< Zones of the catalog, otherwise zones
        QList<const GeoZone*> zones;
        QList<GeoZonePathEvaluator::Path> paths;    ///< Of the planned flight
        QList<GeoZone::Area> areas;
        QList<QList<QGeoCoordinate>> lines;
        QDateTime now;
        QList<double> simplifyTolerances;
        bool circlesAsPolygons = false;
    };

    struct Prepared {
        GeoZoneCatalog catalog;
        GeoZoneFences::Prepared fences;
        QHash<QString, qsizetype> indexes;          ///< Of the entries, by zone key
    };

    static Prepared _prepare(const PrepareInput &input);

    Fences _newFences(const QList<const GeoZone*> &zones, double simplifyTolerance) const;
    int _addFences(const Fences &fences);
    void _setSimplifyTolerances(double maxSimplifyTolerance);
    /// Prepares the zones of the last selection for the vehicle, on the thread pool unless now
    void _prepareSelection(bool now);
    /// Prepares the zones again when a selection was prepared for another vehicle or is being prepared
    void _finishSelection();
    void _applyPrepared(const Prepared &prepared);
    void _select();
    void _updateCapacity();
    void _setBusy(bool busy);
    GeoZoneFenceChoiceModel::Row _row(const GeoZoneFences::Choice &choice) const;
    QString _capacityNote() const;
    QString _parameterWarning() const;
    Vehicle *_vehicle() const;
    void _connectVehicle();
    /// Checks the fences from geozones against the zones, see GeoZoneFenceTags::staleZones
    void _updateStale();
    /// Reads the zones the plan's last import left out, see fenceNote, and those of the vehicle's fence
    void _updateFenceNote();
    void _updateVehicleFenceNote();
    /// Follows the edits of the fences from geozones, which end the fence note
    void _connectFenceEdits();
    /// @return Identifies the zones the selection doesn't fence and why
    QString _notFencedConfirmation() const;
    void _requireUploadConfirmation();
    static QStringList _zoneTexts(const QList<GeoZoneFenceTags::NotFenced> &zones);

    QPointer<GeoFenceController> _geoFenceController;
    std::optional<AreaQuery> _query;                    ///< Of the last updateSelection
    QList<const GeoZone*> _zones;                       ///< Of the last select
    QList<double> _simplifyTolerances = { 0 };
    QHash<QString, bool> _included;                     ///< Zones included or left out by the user, by key
    Prepared _prepared;
    GeoZoneFences::Capacity _capacity;
    GeoZoneFences::Selection _selection;
    int _selectedCount = 0;
    bool _busy = false;
    bool _rejectionHandled = false;                     ///< The capacity was lowered for the fence the vehicle rejected last
    GeoZoneFenceConnections _vehicleConnections;        ///< Of the plan's vehicle, also made again from one of them
    QObject *_fenceConnections = nullptr;
    QObject *_fenceEditConnections = nullptr;           ///< Receiver of the edits of the fences from geozones, replaced to disconnect them
    QTimer _fenceNoteTimer;                             ///< Updates the fence note once after edits
    QJsonObject _settings;                              ///< Of the last updateSelection, see GeoZoneFenceTags::importSettings
    QString _staleText;
    QList<GeoZoneFenceTags::NotFenced> _notFenced;      ///< Of the selection
    QString _fenceNote;
    int _fenceNoteCount = 0;
    QStringList _fenceNoteZones;
    QString _vehicleFenceNote;
    QStringList _vehicleFenceNoteZones;
    GeoZoneMissionWorker *const _worker = nullptr;
    GeoZoneFenceChoiceModel *const _choices = nullptr;
    GeoZoneFenceUpload *const _upload = nullptr;

    static constexpr double _simplifySteps[] = { 0, 10, 25, 50, 100, 250, maxSimplifyTolerance };  ///< Meters, tried up to the maximum tolerance
};
