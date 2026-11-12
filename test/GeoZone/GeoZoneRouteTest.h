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

/// GeoZoneRouteSearch: detours, climbs and descents around the zones of a leg, headless
class GeoZoneRouteTest : public GeoZoneTestBase
{
    Q_OBJECT

private slots:
    void _testConcave_data();
    void _testConcave();
    void _testVertical_data();
    void _testVertical();
    void _testFailures();
    void _testUnknownAltitude();
    void _testSettings();
    void _testMaze();
};
