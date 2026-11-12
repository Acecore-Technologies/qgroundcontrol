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

class GeoZoneParserTest : public GeoZoneTestBase
{
    Q_OBJECT

private slots:
    void _testParseFixtures_data();
    void _testParseFixtures();
    void _testEmptyResponses_data();
    void _testEmptyResponses();
    void _testFilteredAndUnusableFeatures();
    void _testParseED269();
    void _testParseED318();
    void _testParseFaaTfr();
    void _testParseNationalFormats();
    void _testParseNationalVariants();
    void _testParseFaaAirspace();
    void _testFaaSecurityLimits_data();
    void _testFaaSecurityLimits();
    void _testParseOpenAip();
    void _testEd318Writer();
#ifndef GEOZONE_CORE_TEST
    void _testLiveNationalFeeds();
#endif
    void _testParseErrors();
    void _testDurations_data();
    void _testDurations();
    void _testXmlAndPackages();
    void _testZipWriterLimit();
    void _testParseCancel();
    void _testParseScopes();
    void _testParseCacheRoundTrip_data();
    void _testParseCacheRoundTrip();
    void _testParseCacheValidation();
    void _testRingValidation_data();
    void _testRingValidation();
    void _testDroppedGeometry_data();
    void _testDroppedGeometry();
    void _testEdgeInterpolation_data();
    void _testEdgeInterpolation();
    void _testTfrCircles();
    void _testTfrAreaLimits_data();
    void _testTfrAreaLimits();
    void _testAixmArcs();
    void _testRejectedAixmGeometry();
    void _testAixmTitle();
    void _testAixmInvalidBoundary_data();
    void _testAixmInvalidBoundary();
    void _testAixmReferenceSystems_data();
    void _testAixmReferenceSystems();
    void _testParserFuzz();
    void _testFuzzFindings();
    void _testDisplayText();
    void _testLimitTexts_data();
    void _testLimitTexts();
    void _testFlightLevelQnh();
    void _testProviders();
    void _testFeedCatalogue();
};
