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

/// Tests of the feeds requested from services: API keys, OAuth2 tokens, the ED-318 and OpenAIP services, the area of
/// interest and the credentials
class GeoZoneServiceTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _testOpenAipService();
    void _testServiceApiKeys();
    void _testClearForgetsPendingServices();
    void _testSecretsOnlyOfServices();
    void _testServiceOAuth2();
    void _testTokenFollowsService();
    void _testEd318Service();
    void _testServiceAreaOfInterest();
    void _testMapViewAreaOfInterest();
    void _testCredentials();
};
