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

class GeoZoneManagerTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _testManagerLoadPersistClear();
    void _testManagerStoreFailures();
    void _testManagerBackgroundLoad();
    void _testFeedNotModified();
    void _testFeedDatasetLink();
    void _testWfsPaging();
    void _testFeedCompanions();
    void _testReloadChanges();
    void _testSourceValidity();
    void _testDownloadSizeLimit();
    void _testDownloadFailures();
    void _testManagerStartup();
    void _testSettings();
    void _testLabelZonesLimit();
    void _testZoneSearch();
};
