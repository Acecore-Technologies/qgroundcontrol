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

class GeoZoneMonitorTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _testMonitorAlerts();
    void _testMonitorAlertOrder();
    void _testMonitorTimeToEntry();
    void _testTerrainAhead();
    void _testVehicleTelemetry();
    void _testMonitorMultipleVehicles();
    void _testAlertLog();
    void _testAlertHistory();
};
