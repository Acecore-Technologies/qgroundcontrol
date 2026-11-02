/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

/// Base of the geozone tests which also run without the application, in GeoZoneCoreTest. That build defines
/// GEOZONE_CORE_TEST and leaves out the parts which need QGroundControl.
///
/// GEOZONE_TEST(Class) in the source of a test class registers it with QGroundControl's UnitTest, or with
/// GeoZoneCoreTest, which runs those of geoZoneCoreTests(). CMakeLists.txt lists the classes each builds.
#ifdef GEOZONE_CORE_TEST
#include <QtCore/QMap>
#include <QtCore/QObject>

#include <functional>
#include <memory>

using GeoZoneTestBase = QObject;
/// Makes the test of each class registered with GEOZONE_TEST, by class name
using GeoZoneCoreTests = QMap<QString, std::function<std::unique_ptr<QObject>()>>;
GeoZoneCoreTests &geoZoneCoreTests();
#define GEOZONE_TEST(Class) \
    [[maybe_unused]] static const bool Class##Registered = \
        (geoZoneCoreTests().insert(QStringLiteral(#Class), []() -> std::unique_ptr<QObject> { return std::make_unique<Class>(); }), true);
#else
#include "UnitTest.h"
using GeoZoneTestBase = UnitTest;
#define GEOZONE_TEST(Class) static const UnitTestWrapper<Class> Class##Registration(QStringLiteral(#Class), false);
#endif
