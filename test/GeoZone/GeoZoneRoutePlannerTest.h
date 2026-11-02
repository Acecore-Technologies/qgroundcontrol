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

#include <memory>

namespace GeoZoneTestHelpers {
class ScopedMetricUnits;
}

/// Tests of GeoZoneRoutePlanner, the routes around the zones the mission crosses, and of its worker
class GeoZoneRoutePlannerTest : public UnitTest
{
    Q_OBJECT

public:
    GeoZoneRoutePlannerTest();
    ~GeoZoneRoutePlannerTest() override;

private slots:
    void initTestCase();
    void cleanupTestCase();

    void _testRoutePlanner();
    void _testRoutePlannerWorker();

private:
    /// The texts compared are in meters, whatever the locale
    std::unique_ptr<GeoZoneTestHelpers::ScopedMetricUnits> _metricUnits;
};
