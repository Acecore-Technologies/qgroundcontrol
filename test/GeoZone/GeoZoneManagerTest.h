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
    void _testFeedFetchFormatCompanions();
    void _testFeedCancellation();
    void _testRefreshScheduler();
    void _testReloadChanges();
    void _testSourceValidity();
    void _testDownloadSizeLimit();
    void _testDownloadFailures();
    void _testDownloadRedirects();
    void _testServiceApiKeys();
    void _testServiceOAuth2();
    void _testEd318Service();
    void _testServiceAreaOfInterest();
    void _testProvenance();
    void _testIntegrity();
    void _testManagerStartup();
    void _testSettings();
    void _testLabelZonesLimit();
    void _testZoneSearch();
    void _testCatalogSnapshot();
};
