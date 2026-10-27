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
#include <QtPositioning/QGeoCoordinate>
#include <QtPositioning/QGeoShape>

#include "GeoZone.h"

/// A single polygon (with holes) or circle of a GeoZone, flattened for display by map item views.
class GeoZoneMapShape : public QObject
{
    Q_OBJECT

    Q_PROPERTY(GeoZone*         zone        READ zone       CONSTANT)
    Q_PROPERTY(bool             isCircle    READ isCircle   CONSTANT)
    Q_PROPERTY(QGeoShape        geoShape    READ geoShape   CONSTANT)   ///< Polygon including holes, for MapPolygon.geoShape
    Q_PROPERTY(QGeoCoordinate   center      READ center     CONSTANT)   ///< Circle only
    Q_PROPERTY(double           radius      READ radius     CONSTANT)   ///< Circle only, meters

public:
    GeoZoneMapShape(GeoZone *zone, const QGeoShape &polygon, QObject *parent = nullptr)
        : QObject(parent), _zone(zone), _geoShape(polygon) {}
    GeoZoneMapShape(GeoZone *zone, const QGeoCoordinate &center, double radius, QObject *parent = nullptr)
        : QObject(parent), _zone(zone), _isCircle(true), _center(center), _radius(radius) {}

    GeoZone *zone() const { return _zone; }
    bool isCircle() const { return _isCircle; }
    QGeoShape geoShape() const { return _geoShape; }
    QGeoCoordinate center() const { return _center; }
    double radius() const { return _radius; }

private:
    GeoZone *_zone = nullptr;
    bool _isCircle = false;
    QGeoShape _geoShape;
    QGeoCoordinate _center;
    double _radius = 0;
};
