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

/// Tests of GeoZoneReturnModel, the return paths of PX4 and ArduPilot from their parameters
class GeoZoneReturnTest : public GeoZoneTestBase
{
    Q_OBJECT

private slots:
    void _testParameters_data();
    void _testParameters();
    void _testPx4Destination_data();
    void _testPx4Destination();
    void _testPx4Cone_data();
    void _testPx4Cone();
    void _testArduCopterAltitude_data();
    void _testArduCopterAltitude();
    void _testArduCopterPath();
    void _testArduPilotRally_data();
    void _testArduPilotRally();
    void _testArduPlaneReturn_data();
    void _testArduPlaneReturn();
    void _testBreachPath_data();
    void _testBreachPath();
};
