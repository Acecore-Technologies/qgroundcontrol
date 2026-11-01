/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include "GeoZoneGeoid.h"
#include "GeoZoneGreatCircle.h"
#include "GeoZonePredicates.h"
#include "GeoZoneProjection.h"
#include "GeoZoneSimplify.h"

// Planar geometry helpers for geozone and fence polygons, in namespace GeoZoneGeometry:
// - GeoZoneProjection.h: the local projection to meters the other units work in
// - GeoZonePredicates.h: containment, intersection, distance and interval tests
// - GeoZoneSimplify.h: outward simplification of polygons
// - GeoZoneGreatCircle.h: great circles of zone edges and path legs
// - GeoZoneGeoid.h: geoid heights
// Shared planar vector helpers are in GeoZoneVector.h.
