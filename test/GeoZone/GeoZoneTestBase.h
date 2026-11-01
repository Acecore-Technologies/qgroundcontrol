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
#ifdef GEOZONE_CORE_TEST
#include <QtCore/QObject>
using GeoZoneTestBase = QObject;
#else
#include "UnitTest.h"
using GeoZoneTestBase = UnitTest;
#endif
