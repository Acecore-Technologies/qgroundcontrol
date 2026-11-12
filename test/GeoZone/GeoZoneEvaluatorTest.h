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

/// GeoZonePathEvaluator, the catalog queries and the alert tracker, headless
class GeoZoneEvaluatorTest : public GeoZoneTestBase
{
    Q_OBJECT

private slots:
    void _testPathHits();
    void _testCrossings();
    void _testAltitudes();
    void _testUnknownTerrain();
    void _testAuthorisationScope();
    void _testAuthorisationLimits_data();
    void _testAuthorisationLimits();
    void _testProhibitedAuthorisations();
    void _testGreatCircles();
    void _testApproximations();
    void _testSchedules();
    void _testAntimeridianQueries();
    void _testAlertThresholds();
    void _testThresholdWarningTimes();
    void _testActivationTracker();
};
