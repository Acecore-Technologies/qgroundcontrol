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

/// Tests of GeoZoneManager: its startup, its settings, the labels and search of its zones, the snapshots of its catalog
/// and the zones several sources publish
class GeoZoneManagerTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _testManagerStartup();
    void _testSettings();
    void _testLabelZonesLimit();
    void _testZoneSearch();
    void _testCatalogSnapshot();
    void _testMergedSources();
};
