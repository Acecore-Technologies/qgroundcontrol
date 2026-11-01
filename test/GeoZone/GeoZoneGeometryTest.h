/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include "GeoZoneTestBase.h"

class GeoZoneGeometryTest : public GeoZoneTestBase
{
    Q_OBJECT

private slots:
    void _testIsActiveAt();
    void _testDaylightEvents();
    void _testIsActiveDuring();
    void _testOutwardSimplification();
    void _testSpatialIndex();
    void _testExclusionShapes();
    void _testFenceCapacity();
    void _testFenceSelection();
    void _testContainsAndPaths();
    void _testAltitudeLimits();
    void _testVerticalOverlap_data();
    void _testVerticalOverlap();
    void _testFlightLevels_data();
    void _testFlightLevels();
    void _testAreaIntersection();
    void _testShapeSimplification();
#ifndef GEOZONE_CORE_TEST
    void _testMapLayerTriangulation();
#endif
    void _testAntimeridian();
    void _testBoxAcrossAntimeridian();
    void _testProjectionContinuity();
    void _testConcurrentQueries();
};
