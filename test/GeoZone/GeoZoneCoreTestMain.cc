/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneGeometryTest.h"
#include "GeoZoneParserTest.h"
#include "GeoZoneReturnTest.h"

#include <QtCore/QCoreApplication>
#include <QtTest/QTest>

/// Runs the geozone tests which only need the GeoZoneCore library, without QGroundControl
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    // Settings the core may write stay apart from those of QGroundControl
    QCoreApplication::setOrganizationName(QStringLiteral("QGroundControl"));
    QCoreApplication::setApplicationName(QStringLiteral("GeoZoneCoreTest"));

    int status = 0;
    {
        GeoZoneParserTest test;
        status |= QTest::qExec(&test, argc, argv);
    }
    {
        GeoZoneGeometryTest test;
        status |= QTest::qExec(&test, argc, argv);
    }
    {
        GeoZoneReturnTest test;
        status |= QTest::qExec(&test, argc, argv);
    }
    return status;
}
