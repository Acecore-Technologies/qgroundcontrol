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

/// Tests of GeoZoneOperationalIntent, the 4D volumes of the operational intent of a planned flight in the ASTM
/// F3548-21 data model
class GeoZoneIntentTest : public GeoZoneTestBase
{
    Q_OBJECT

private slots:
    void _testVolumes_data();
    void _testVolumes();
    void _testVolumeCount();
    void _testAltitudes();
    void _testErrors();
    void _testJson();
};
