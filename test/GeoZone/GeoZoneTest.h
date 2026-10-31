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
    void _testParseErrors();
    void _testIsActiveAt();
    void _testDisplayText();
    void _testExclusionShapes();
    void _testManagerLoadPersistClear();
    void _testGeoFenceImport();
    void _testLabelZonesLimit();
};
