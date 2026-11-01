/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include "UnitTest.h"

class GeoZoneGeometryTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _testIsActiveAt();
    void _testDaylightEvents();
    void _testIsActiveDuring();
    void _testOutwardSimplification();
    void _testSpatialIndex();
    void _testExclusionShapes();
    void _testContainsAndPaths();
    void _testAltitudeLimits();
    void _testAreaIntersection();
    void _testShapeSimplification();
    void _testMapLayerTriangulation();
    void _testAntimeridian();
    void _testBoxAcrossAntimeridian();
    void _testProjectionContinuity();
    void _testConcurrentQueries();
};
