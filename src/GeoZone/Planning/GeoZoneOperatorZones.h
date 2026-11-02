/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include "GeoZone.h"

#include <QtCore/QObject>
#include <QtCore/QVariantList>
#include <QtCore/QVariantMap>
#include <QtPositioning/QGeoRectangle>

class GeoZoneSourceRepository;
class QGCMapCircle;
class QGCMapPolygon;

/// The zones the operator draws in the Plan view: polygons or circles with a name, a restriction and vertical limits,
/// kept as the ED-318 file "My zones" (see GeoZoneEd318Writer) and loaded as a source of the repository like any other,
/// so they are shown, checked, alerted, imported as fences and exported in packs. One zone is edited at a time, with
/// the QGCMapPolygon or QGCMapCircle the map edits.
class GeoZoneOperatorZones : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("QGCMapCircle.h")
    Q_MOC_INCLUDE("QGCMapPolygon.h")

    Q_PROPERTY(QVariantList     zones           READ zones          NOTIFY zonesChanged FINAL)      ///< Maps with name, restriction (GeoZone::Restriction), restrictionText and circle
    Q_PROPERTY(QStringList      restrictionNames READ restrictionNames CONSTANT FINAL)              ///< Of the restrictions a zone can have, from Prohibited to NoRestriction
    Q_PROPERTY(bool             editing         READ editing        NOTIFY editingChanged FINAL)
    Q_PROPERTY(int              editIndex       READ editIndex      NOTIFY editingChanged FINAL)    ///< Of the zone edited, -1 for a new one
    Q_PROPERTY(bool             editCircle      READ editCircle     NOTIFY editingChanged FINAL)    ///< The zone edited is the circle, otherwise the polygon
    Q_PROPERTY(QVariantMap      editProperties  READ editProperties NOTIFY editingChanged FINAL)    ///< See properties()
    Q_PROPERTY(QGCMapPolygon*   polygon         READ polygon        CONSTANT FINAL)
    Q_PROPERTY(QGCMapCircle*    circle          READ circle         CONSTANT FINAL)

public:
    explicit GeoZoneOperatorZones(GeoZoneSourceRepository *repository, QObject *parent = nullptr);
    ~GeoZoneOperatorZones() override;

    /// Of the zones, in the store's directory, also the key of their source
    QString fileName() const;
    /// Reads the zones from the file
    void load();

    const QList<GeoZone::Data> &zoneData() const { return _zones; }
    QVariantList zones() const;
    static QStringList restrictionNames();
    bool editing() const { return _editing; }
    int editIndex() const { return _editIndex; }
    bool editCircle() const { return _editCircle; }
    QVariantMap editProperties() const { return _editProperties; }
    QGCMapPolygon *polygon() const { return _polygon; }
    QGCMapCircle *circle() const { return _circle; }

    /// Starts drawing a new polygon or circle in the middle of the map view
    Q_INVOKABLE void startPolygon(const QGeoRectangle &view);
    Q_INVOKABLE void startCircle(const QGeoRectangle &view);
    /// Starts editing the zone at the index
    Q_INVOKABLE void startEdit(int index);
    Q_INVOKABLE void cancelEdit();
    /// Saves the zone edited with the properties, see properties(), and ends the edit
    /// @return false when the zone can't be saved, failed tells why
    Q_INVOKABLE bool saveEdit(const QVariantMap &properties);
    /// Removes the zone at the index, and the source once no zone is left
    Q_INVOKABLE bool removeZone(int index);

    /// Replaces the zones, writing the file and loading it as a source
    /// @return false when the file can't be written or loaded, errorString tells why
    bool setZones(const QList<GeoZone::Data> &zones, QString &errorString);

    /// @return The editable properties of a zone: name, restriction (GeoZone::Restriction), message, lower and upper
    ///         (meters, NaN without) and lowerReference and upperReference ("AGL" or "AMSL")
    static QVariantMap properties(const GeoZone::Data &zone);
    /// @return A zone with the properties and the volume of the shape, see properties()
    /// @param identifier Kept across edits, so that authorisations and alerts follow the zone
    static GeoZone::Data zoneFromProperties(const QVariantMap &properties, const GeoZone::Volume &shape, const QString &identifier);

    static constexpr const char *title = QT_TRANSLATE_NOOP("GeoZoneOperatorZones", "My zones");

signals:
    void zonesChanged();
    void editingChanged();
    void failed(const QString &message);

private:
    void _setEditing(bool editing, int index, bool circle, const QVariantMap &properties);
    /// Forgets the zones when the user removed their source
    void _sourceRemoved();

    GeoZoneSourceRepository *_repository = nullptr;
    QList<GeoZone::Data> _zones;
    QGCMapPolygon *_polygon = nullptr;
    QGCMapCircle *_circle = nullptr;
    bool _editing = false;
    int _editIndex = -1;
    bool _editCircle = false;
    QVariantMap _editProperties;
    bool _saving = false;
};
