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
#include <QtCore/QString>

class GeoFenceController;
class QJsonObject;

/// Name and origin of a polygon or circle of a GeoFenceController, kept by GeoZone rather than in QGCFencePolygon and
/// QGCFenceCircle: a child object of the fence, so it goes with the fence. Copies of the fence which QGroundControl
/// makes, like those of the vehicle's GeoFenceManager, don't have it, like fences read back from a vehicle.
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

signals:
    void nameChanged();
    void geoZoneChanged();

private:
    explicit GeoZoneFenceTag(QObject *fence);

    QString _name;
    bool _geoZone = false;
};

/// The fence tags for QML, as the GeoZoneFenceTags singleton, and in plan files. A plan file keeps the tag of a fence
/// as the optional "name" and "geoZone" keys of the fence's entry, which other ground stations ignore.
class GeoZoneFenceTags : public QObject
{
    Q_OBJECT

public:
    explicit GeoZoneFenceTags(QObject *parent = nullptr);

    /// @return Tag of the fence, added when it has none
    Q_INVOKABLE GeoZoneFenceTag *tag(QObject *fence) const;

    /// Deletes many polygons and circles at once, with one reset of each list rather than a row removal per fence,
    /// which matters for the many fences added from geozones
    /// @param geoZoneOnly Only the fences added from geozones, otherwise all of them
    Q_INVOKABLE static void deleteFences(GeoFenceController *geoFenceController, bool geoZoneOnly);

    /// Adds the tags of the fences to their entries in the JSON of a plan, saved by PlanMasterController
    static void save(GeoFenceController *geoFenceController, QJsonObject &planJson);

    /// Sets the tags of the fences of a plan loaded from its JSON
    static void load(GeoFenceController *geoFenceController, const QJsonObject &planJson);
};
