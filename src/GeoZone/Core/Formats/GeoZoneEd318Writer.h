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

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>

/// Writes zones as EUROCAE ED-318 GeoJSON, with the field names GeoZoneParser reads back, like the zones operators
/// draw (see GeoZoneOperatorZones). Polygon volumes are written as polygons with their outer ring counterclockwise,
/// circles as points with a circle extent, several volumes as a geometry collection; each geometry has its vertical
/// limits as its layer. Schedules, U-space classes and extended properties aren't written.
namespace GeoZoneEd318Writer {

/// @return The ED-318 code of a restriction, like REQ_AUTHORIZATION, empty for Unknown
QString restrictionCode(GeoZone::Restriction restriction);

/// @return The ED-318 feature of a zone
QJsonObject feature(const GeoZone::Data &zone);

/// @return An ED-318 feature collection of the zones with the dataset's name, provider and issue time
QByteArray document(const QList<GeoZone::Data> &zones, const QString &title, const QString &provider, const QDateTime &issued);

} // namespace GeoZoneEd318Writer
