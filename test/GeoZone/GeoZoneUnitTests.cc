/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFlightTest.h"
#include "GeoZoneGeometryTest.h"
#include "GeoZoneIntentTest.h"
#include "GeoZoneManagerTest.h"
#include "GeoZoneMonitorTest.h"
#include "GeoZoneParserTest.h"
#include "GeoZonePlanningTest.h"
#include "GeoZoneReturnTest.h"

// Registered here rather than in UnitTestList.cc, so the tests of the module need no changes to QGroundControl's
// list. The wrappers only add the tests to the list of UnitTest, which runs them with the others.
namespace {

const UnitTestWrapper<GeoZoneFlightTest> geoZoneFlightTest(QStringLiteral("GeoZoneFlightTest"), false);
const UnitTestWrapper<GeoZoneGeometryTest> geoZoneGeometryTest(QStringLiteral("GeoZoneGeometryTest"), false);
const UnitTestWrapper<GeoZoneIntentTest> geoZoneIntentTest(QStringLiteral("GeoZoneIntentTest"), false);
const UnitTestWrapper<GeoZoneManagerTest> geoZoneManagerTest(QStringLiteral("GeoZoneManagerTest"), false);
const UnitTestWrapper<GeoZoneMonitorTest> geoZoneMonitorTest(QStringLiteral("GeoZoneMonitorTest"), false);
const UnitTestWrapper<GeoZoneParserTest> geoZoneParserTest(QStringLiteral("GeoZoneParserTest"), false);
const UnitTestWrapper<GeoZonePlanningTest> geoZonePlanningTest(QStringLiteral("GeoZonePlanningTest"), false);
const UnitTestWrapper<GeoZoneReturnTest> geoZoneReturnTest(QStringLiteral("GeoZoneReturnTest"), false);

} // namespace
