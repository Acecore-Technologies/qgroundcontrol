/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneAppTests.h"
#include "UnitTest.h"

// Registered here rather than in UnitTestList.cc, so the tests of the module need no changes to QGroundControl's
// list. The wrappers only add the tests to the list of UnitTest, which runs them with the others. The classes are
// those of the lists of CMakeLists.txt, which also adds their CTest tests.
namespace {

#define GEOZONE_REGISTER_TEST(Test) const UnitTestWrapper<Test> Test##Registration(QStringLiteral(#Test), false);
GEOZONE_APP_TESTS(GEOZONE_REGISTER_TEST)
#undef GEOZONE_REGISTER_TEST

} // namespace
