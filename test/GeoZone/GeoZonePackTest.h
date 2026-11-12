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

/// Tests of the offline packs: their export and import, their signatures, the services of their sources and the names
/// of the stored copies they hold
class GeoZonePackTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _testOfflinePack();
    void _testPackServiceTrust();
    void _testPackExtraction();
    void _testStoredFileNames();
    void _testSignedPacks();
    void _testPacksWithoutSigning();
};
