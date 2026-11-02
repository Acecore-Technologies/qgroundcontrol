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

/// Tests of the path of missions: GeoZoneMissionPath, GeoZoneMissionPathBuilder and GeoZoneMissionJumps headless, and
/// in QGroundControl the reading of missions by GeoZoneMissionReader
class GeoZoneMissionPathTest : public GeoZoneTestBase
{
    Q_OBJECT

private slots:
    void _testMissionPathTimes();
    void _testMissionJumpWalk();
};
