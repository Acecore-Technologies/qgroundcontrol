/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>

class GeoFenceController;
class GeoZone;

/// Name and origin of a polygon or circle of a GeoFenceController, kept by GeoZone rather than in QGCFencePolygon and
/// QGCFenceCircle: a child object of the fence, so it goes with the fence. Copies of the fence which QGroundControl
/// makes, like those of the vehicle's GeoFenceManager, don't have it, like fences read back from a vehicle.
/// Fences added from a zone keep its key, content hash and validity end, to tell when the zone changed since.
class GeoZoneFenceTag : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString  name    READ name       WRITE setName       NOTIFY nameChanged      FINAL)  ///< Optional label, like the zone of a fence added from a geozone
    Q_PROPERTY(bool     geoZone READ geoZone    WRITE setGeoZone    NOTIFY geoZoneChanged   FINAL)  ///< Added from a geozone

public:
    /// @return Tag of the fence, added when it has none
    static GeoZoneFenceTag *of(QObject *fence);

    /// @return Tag of the fence, null when it has none
    static const GeoZoneFenceTag *find(const QObject *fence);

    /// @return true if the fence was added from a geozone
    static bool isGeoZone(const QObject *fence);

    QString name() const { return _name; }
    void setName(const QString &name);
    bool geoZone() const { return _geoZone; }
    void setGeoZone(bool geoZone);

    /// Marks the fence as added from the zone, with its name, key, content hash and validity end
    void setZone(const GeoZone *zone);
    QString zoneKey() const { return _zoneKey; }        ///< See GeoZone::key, empty when unknown
    QByteArray contentHash() const { return _contentHash; }
    QDateTime validTo() const { return _validTo; }      ///< When the zone stops applying, invalid without end

    /// @return The end of the last period the zone applies in, invalid when it applies without end
    static QDateTime validityEnd(const GeoZone *zone);

signals:
    void nameChanged();
    void geoZoneChanged();

private:
    friend class GeoZoneFenceTags;     // Loads the zone of fences from plans

    explicit GeoZoneFenceTag(QObject *fence);

    QString _name;
    bool _geoZone = false;
    QString _zoneKey;
    QByteArray _contentHash;
    QDateTime _validTo;
};

/// The fence tags for QML, as the GeoZoneFenceTags singleton, and in plan files. A plan file keeps the tag of a fence
/// as the optional "name", "geoZone", "zoneKey", "contentHash" and "validTo" keys of the fence's entry, and the
/// settings the fences from geozones were selected with as "geoZoneImport" of the GeoFence, with the zones they left
/// out ("notFenced"), which other ground stations ignore. The controller keeps the settings while it keeps the fences,
/// see importSettings.
class GeoZoneFenceTags : public QObject
{
    Q_OBJECT

public:
    explicit GeoZoneFenceTags(QObject *parent = nullptr);

    /// @return Tag of the fence, added when it has none
    Q_INVOKABLE GeoZoneFenceTag *tag(QObject *fence) const;

    /// Deletes many polygons and circles at once, with one reset of each list rather than a row removal per fence,
    /// which matters for the many fences added from geozones
    /// @param geoZoneOnly Only the fences added from geozones, otherwise all of them, with the import settings
    Q_INVOKABLE static void deleteFences(GeoFenceController *geoFenceController, bool geoZoneOnly);

    /// Adds the tags of the fences to their entries in the JSON of a plan, saved by PlanMasterController
    static void save(GeoFenceController *geoFenceController, QJsonObject &planJson);

    /// Sets the tags of the fences of a plan loaded from its JSON
    static void load(GeoFenceController *geoFenceController, const QJsonObject &planJson);

    /// Zone of fences from geozones which no longer matches them
    struct StaleZone {
        enum class Reason {
            Changed,    ///< Its data changed since the fences were added
            Removed,    ///< No longer loaded
            Expired,    ///< Its last period ended
        };
        QString name;
        Reason reason = Reason::Changed;

        friend bool operator==(const StaleZone &, const StaleZone &) = default;
    };

    /// @return The zones of the fences from geozones which changed since the fences were added, are no longer among the
    ///         zones or expired by now, once each. Fences of plans saved without the zone keys aren't checked.
    static QList<StaleZone> staleZones(GeoFenceController *geoFenceController, const QList<GeoZone*> &zones, const QDateTime &now);
    static constexpr int maxListedStaleZones = 5;

    /// @return Why the geozone fences are outdated, like "The zones of geozone fences changed: A (changed), B
    ///         (removed). Rebuild the geozone fences.", empty without stale zones
    static QString staleText(const QList<StaleZone> &staleZones);

    /// @return The settings the fences from geozones of the controller were last selected with, see
    ///         GeoZoneFenceImporter::rebuild, empty without. They are kept with the fences they were set for: once the
    ///         controller replaces all its fences, like for a new plan or the fence loaded from the vehicle, they are
    ///         empty, with the zones not fenced, see notFenced.
    static QJsonObject importSettings(const GeoFenceController *geoFenceController);
    static void setImportSettings(GeoFenceController *geoFenceController, const QJsonObject &settings);

    /// Zone which the last selection of fences from geozones left out, kept with the import settings
    struct NotFenced {
        QString name;
        QString reason;     ///< See GeoZoneFences::statusText

        friend bool operator==(const NotFenced &, const NotFenced &) = default;
    };
    static constexpr int maxListedNotFenced = 5;    ///< Zones named by notFencedText, the others are counted

    /// Records the zones the last selection left out, all of them, with the fences from geozones it added
    /// @param count Number of zones left out when the zones are only some of them
    static void setNotFenced(GeoFenceController *geoFenceController, const QList<NotFenced> &zones, int count);
    /// @return The zones the last selection left out, while the fences from geozones are the ones it added. Nothing once
    ///         those changed, or without such a selection.
    /// @param count Set to the number of zones left out, also those which plans saved by earlier versions didn't keep
    static QList<NotFenced> notFenced(GeoFenceController *geoFenceController, int *count = nullptr);
    /// @return Like "3 geozones are not fenced: A (No room left in the vehicle fence), B (Left out by you) and 1 more.",
    ///         empty without zones. A summary: what shows it also gives the way to see all of them.
    static QString notFencedText(const QList<NotFenced> &zones, int count, int maxListed = maxListedNotFenced);
    /// @return Identifies the polygons and circles from geozones of the controller, in their order
    static QString geoZoneFencesFingerprint(GeoFenceController *geoFenceController);
};
