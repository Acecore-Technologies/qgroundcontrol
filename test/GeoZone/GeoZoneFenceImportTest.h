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

#include <QtCore/QTemporaryDir>

#include <memory>

namespace GeoZoneTestHelpers {
class ScopedMetricUnits;
}

/// Tests of the fences made from geozones: their import (GeoZoneFenceImporter), the zones they exclude from the
/// mission check, the selection of the zones, outdated fences and their upload to the vehicle (GeoZoneFenceUpload)
class GeoZoneFenceImportTest : public UnitTest
{
    Q_OBJECT

public:
    GeoZoneFenceImportTest();
    ~GeoZoneFenceImportTest() override;

private slots:
    void initTestCase();
    void cleanupTestCase();

    void _testFenceExcludes();
    void _testGeoFenceImport();
    void _testGeoFenceSelection();
    void _testGeoFenceNotFenced();
    void _testGeoFenceUploadConfirmation();
    void _testGeoFenceRecords();
    void _testGeoFenceActiveTime();
    void _testGeoFenceStale();
    void _testGeoFenceRebuildSettings();
    void _testGeoFenceSettingsReplaced();
    void _testGeoFenceUpload();
    void _testGeoFenceUploadChanges();
    void _testGeoFenceUploadDetached();
    void _testFenceWarningVehicle();
    void _testFenceUploadRejected();
    void _testFenceOverrideCommands();

private:
    /// Connects a MockLink vehicle of the autopilot, see _readyVehicle
    Vehicle *_connectVehicle(MAV_AUTOPILOT autopilot);
    /// @return The active vehicle once it sent its plan and parameters, null when it doesn't in time
    Vehicle *_readyVehicle();

    /// The texts compared are in meters, whatever the locale
    std::unique_ptr<GeoZoneTestHelpers::ScopedMetricUnits> _metricUnits;
    /// Keeps the fences recorded for the vehicles of the tests, see GeoZoneFenceUpload::setRecordsFile
    std::unique_ptr<QTemporaryDir> _recordsDir;
};
