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

#ifndef GEOZONE_CORE_TEST
#include <memory>

namespace GeoZoneTestHelpers {
class ScopedMetricUnits;
}
#endif

/// Tests of the path of missions: GeoZoneMissionPath, GeoZoneMissionPathBuilder and GeoZoneMissionJumps headless, and
/// in QGroundControl the reading of missions by GeoZoneMissionReader
class GeoZoneMissionPathTest : public GeoZoneTestBase
{
    Q_OBJECT

#ifndef GEOZONE_CORE_TEST
public:
    GeoZoneMissionPathTest();
    ~GeoZoneMissionPathTest() override;
#endif

private slots:
#ifndef GEOZONE_CORE_TEST
    void initTestCase();
    void cleanupTestCase();
#endif

    void _testMissionPathTimes();
    void _testMissionJumpWalk();
    void _testFlightEndsAtReturn();
#ifndef GEOZONE_CORE_TEST
    void _testMissionCoverage();
    void _testMissionJumps();
    void _testMissionContingencies();
    void _testMissionReturnParameters();

private:
    /// The texts compared are in meters, whatever the locale
    std::unique_ptr<GeoZoneTestHelpers::ScopedMetricUnits> _metricUnits;
#endif
};
