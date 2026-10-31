/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QLoggingCategory>
#include <QtCore/QObject>
#include <QtCore/QStringList>
#include <QtPositioning/QGeoRectangle>

#include "GeoZone.h"

Q_DECLARE_LOGGING_CATEGORY(GeoZoneManagerLog)

class GeoZoneShape;
class QmlObjectListModel;
class QTimer;

/// Loads UAS geographical zones from EUROCAE ED-269 / ED-318 files for display on the maps and for
/// conversion to GeoFence exclusion areas. The most recently loaded file is kept in the application
/// data folder and automatically reloaded on startup.
class GeoZoneManager : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("QmlObjectListModel.h")

    Q_PROPERTY(QmlObjectListModel*  zones               READ zones              CONSTANT)
    Q_PROPERTY(QString              sourceName          READ sourceName         NOTIFY zonesChanged)
    Q_PROPERTY(QString              formatName          READ formatName         NOTIFY zonesChanged)
    Q_PROPERTY(int                  skippedCount        READ skippedCount       NOTIFY zonesChanged)
    Q_PROPERTY(QGeoRectangle        boundingBox         READ boundingBox        NOTIFY zonesChanged)   ///< Bounding box of all zones
    Q_PROPERTY(bool                 visible             READ visible            WRITE setVisible NOTIFY visibleChanged)
    Q_PROPERTY(QStringList          fileDialogFilters   READ fileDialogFilters  CONSTANT)

public:
    static constexpr int maxLabelCount = 200;   ///< Limits the number of label items created on a map

    /// @param storageDir Directory in which a copy of the loaded file is kept. Empty for the default application data location.
    explicit GeoZoneManager(const QString &storageDir = QString(), QObject *parent = nullptr);
    ~GeoZoneManager();

    static GeoZoneManager *instance();

    /// Loads zones from an ED-269 or ED-318 file, replacing any previously loaded zones. Errors are shown to the user.
    Q_INVOKABLE bool loadFile(const QString &fileName);
    bool loadFile(const QString &fileName, QString &errorString);

    /// Removes all zones and the stored copy of the file
    Q_INVOKABLE void clear();

    /// @return Zones to label in the map region: those centered in it, limited to maxLabelCount
    ///         preferring the most restrictive
    Q_INVOKABLE QList<QObject*> labelZones(const QGeoRectangle &region) const;

    /// @return Zones matching one of the restrictions which intersect the area. An invalid area matches all zones.
    QList<GeoZone*> zonesInArea(const QList<GeoZone::Restriction> &restrictions, const QGeoRectangle &area) const;

    QmlObjectListModel *zones() const { return _zones; }
    /// Polygons and circles of all zones for map display. Volumes sharing a footprint have one shape.
    const QList<GeoZoneShape*> &shapes() const { return _shapes; }
    QString sourceName() const { return _sourceName; }
    QString formatName() const { return _formatName; }
    int skippedCount() const { return _skippedCount; }
    QGeoRectangle boundingBox() const { return _boundingBox; }
    bool visible() const { return _visible; }
    void setVisible(bool visible);
    QStringList fileDialogFilters() const;

    /// Path of the stored copy of the most recently loaded file
    QString storedFilePath() const;

signals:
    void zonesAboutToChange();  ///< Zones and shapes are about to be deleted
    void zonesChanged();
    void visibleChanged(bool visible);

private slots:
    void _updateActive();

private:
    bool _loadStoredFile();
    void _setZones(const QList<GeoZone::Data> &zones);
    void _clearZones();

    QString _storageDir;
    QmlObjectListModel *_zones = nullptr;
    QList<GeoZoneShape*> _shapes;
    QTimer *_activeTimer = nullptr;
    QString _sourceName;
    QString _formatName;
    int _skippedCount = 0;
    QGeoRectangle _boundingBox;
    bool _visible = true;

    static constexpr const char *_settingsGroup = "GeoZoneManager";
    static constexpr const char *_visibleKey = "visible";
    static constexpr const char *_sourceNameKey = "sourceName";
    static constexpr const char *_storedFileName = "geozones.json";
    static constexpr int _activeUpdateIntervalMSecs = 60 * 1000;
};
