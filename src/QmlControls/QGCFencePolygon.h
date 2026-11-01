/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include "QGCMapPolygon.h"

/// The QGCFencePolygon class provides a polygon used by GeoFence support.
class QGCFencePolygon : public QGCMapPolygon
{
    Q_OBJECT

public:
    QGCFencePolygon(bool inclusion, QObject* parent = nullptr);
    QGCFencePolygon(const QGCFencePolygon& other, QObject* parent = nullptr);

    const QGCFencePolygon& operator=(const QGCFencePolygon& other);

    Q_PROPERTY(bool inclusion READ inclusion WRITE setInclusion NOTIFY inclusionChanged)
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged FINAL)           ///< Optional label, like the zone of a fence added from a geozone
    Q_PROPERTY(bool geoZone READ geoZone WRITE setGeoZone NOTIFY geoZoneChanged FINAL)  ///< Added from a geozone

    /// Saves the QGCFencePolygon to the json object.
    ///     @param json Json object to save to
    void saveToJson(QJsonObject& json);

    /// Load a QGCFencePolygon from json
    ///     @param json Json object to load from
    ///     @param required true: no polygon in object will generate error
    ///     @param errorString Error string if return is false
    /// @return true: success, false: failure (errorString set)
    bool loadFromJson(const QJsonObject& json, bool required, QString& errorString);

    // Property methods

    bool inclusion      (void) const { return _inclusion; }
    void setInclusion   (bool inclusion);
    QString name        (void) const { return _name; }
    void setName        (const QString& name);
    bool geoZone        (void) const { return _geoZone; }
    void setGeoZone     (bool geoZone);

signals:
    void nameChanged        (const QString& name);
    void geoZoneChanged     (bool geoZone);
    void inclusionChanged   (bool inclusion);

private slots:
    void _setDirty(void);

private:
    void _init(void);

    bool _inclusion;
    QString _name;
    bool _geoZone = false;

    static constexpr int _jsonCurrentVersion = 1;

    static constexpr const char* _jsonInclusionKey = "inclusion";
    // Optional, not used by other ground stations
    static constexpr const char* _jsonNameKey = "name";
    static constexpr const char* _jsonGeoZoneKey = "geoZone";
};
