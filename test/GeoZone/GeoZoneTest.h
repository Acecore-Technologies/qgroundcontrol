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

class GeoZoneTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _testParseED269();
    void _testParseED318();
    void _testParseFaaTfr();
    void _testParseErrors();
    void _testIsActiveAt();
    void _testDaylightEvents();
    void _testIsActiveDuring();
    void _testOutwardSimplification();
    void _testSpatialIndex();
    void _testDisplayText();
    void _testExclusionShapes();
    void _testManagerLoadPersistClear();
    void _testManagerStoreFailures();
    void _testManagerBackgroundLoad();
    void _testFeedNotModified();
    void _testSourceValidity();
    void _testGeoFenceImport();
    void _testGeoFenceImportPreview();
    void _testContainsAndPaths();
    void _testAltitudeLimits();
    void _testAreaIntersection();
    void _testMissionCheck();
    void _testMonitorAlerts();
    void _testRoutePlanner();
    void _testMonitorAlertOrder();
    void _testMonitorTimeToEntry();
    void _testMonitorMultipleVehicles();
    void _testAlertLog();
    void _testProfileBands();
    void _testDownloadSizeLimit();
    void _testDownloadFailures();
    void _testManagerStartup();
    void _testParserFuzz();
    void _testSettings();
    void _testLabelZonesLimit();
    void _testShapeSimplification();
    void _testMapModel();
    void _testMapModelShapeLimit();
    void _testZoneSearch();
};
