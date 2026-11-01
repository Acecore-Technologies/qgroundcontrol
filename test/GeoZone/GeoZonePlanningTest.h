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

class GeoZonePlanningTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _testPathHits();
    void _testEvaluatorCrossings();
    void _testEvaluatorAltitudes();
    void _testEvaluatorSchedules();
    void _testFenceExcludes();
    void _testAntimeridianQueries();
    void _testMissionCheck();
    void _testMissionCoverage();
    void _testMissionJumps();
    void _testMissionContingencies();
    void _testMissionReturnParameters();
    void _testAuthorisations();
    void _testBriefing();
    void _testBriefingExport();
    void _testRoutePlanner();
    void _testProfileBands();
    void _testGeoFenceImport();
    void _testGeoFenceImportPreview();
    void _testMapModel();
    void _testMapModelShapeLimit();
};
