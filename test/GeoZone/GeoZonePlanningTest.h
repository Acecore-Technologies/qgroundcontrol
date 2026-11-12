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

class GeoZonePlanningTest : public UnitTest
{
    Q_OBJECT

public:
    GeoZonePlanningTest();
    ~GeoZonePlanningTest() override;

private slots:
    void initTestCase();
    void cleanupTestCase();

    void _testGuidedPathWarning();
    void _testApproximationReports();
    void _testAntimeridianProfile();
    void _testMissionCheck();
    void _testMissionControllerSwitch();
    void _testAuthorisations();
    void _testNotifications();
    void _testBriefingDocuments();
    void _testBriefingPrint();
    void _testBriefingExport();
    void _testPlanAudit();
    void _testPlanSave();
    void _testUploadReasons();
    void _testUploadReasonDetails();
    void _testPlannedStartPerPlan();
    void _testProfileBands();
    void _testOperatorZones();
    void _testOperatorZonesFile();
    void _testOperatorZonesRemovalFailure();
    void _testMapModel();
    void _testMapModelShapeLimit();
    void _testMapModelLabels();
    void _testMapModelFilters();

private:
    /// The texts compared are in meters, whatever the locale
    std::unique_ptr<GeoZoneTestHelpers::ScopedMetricUnits> _metricUnits;
};
