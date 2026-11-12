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

/// Tests of the geozone sources of GeoZoneSourceRepository: loading, storing and reloading them, the downloads of the
/// feeds with their paging, companions and refreshes, the validity of their data, their provenance and integrity
class GeoZoneRepositoryTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _testManagerLoadPersistClear();
    void _testManagerStoreFailures();
    void _testStoredCopyKeptUntilIndexSaved();
    void _testInterruptedCopiesRemoved();
    void _testIndexEntriesValidated_data();
    void _testIndexEntriesValidated();
    void _testManagerBackgroundLoad();
    void _testFeedNotModified();
    void _testFeedDatasetLink();
    void _testWfsPaging();
    void _testPageMerge();
    void _testArcGisPaging();
    void _testPageCap();
    void _testFeedCompanions();
    void _testFeedFetchFormatCompanions();
    void _testCompanionsWriteFailure();
    void _testFeedCancellation();
    void _testRefreshScheduler();
    void _testFeedCatalogueOverlays();
    void _testFeedCatalogueUrlRules();
    void _testRefreshAfterOffline();
    void _testRetryWhenOnline();
    void _testReloadChanges();
    void _testAiracCycles_data();
    void _testAiracCycles();
    void _testAiracExpiry();
    void _testSourceValidity();
    void _testFeedAgeFromLastCheck();
    void _testDownloadSizeLimit();
    void _testPagedSizeLimit();
    void _testDownloadWithoutFile();
    void _testDownloadFilesRemoved();
    void _testDownloadFailures();
    void _testDownloadRedirects();
    void _testProvenance();
    void _testIntegrity();
};
