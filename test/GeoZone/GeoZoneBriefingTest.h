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

/// GeoZoneBriefing and the versions of the sources it reports, headless
class GeoZoneBriefingTest : public GeoZoneTestBase
{
    Q_OBJECT

private slots:
    void _testBriefing();
};
