/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneTestBase.h"

#include <QtCore/QCoreApplication>
#include <QtTest/QTest>

#include <cstdio>

GeoZoneCoreTests &geoZoneCoreTests()
{
    static GeoZoneCoreTests tests;
    return tests;
}

/// Runs the geozone tests which only need the GeoZoneCore library, without QGroundControl: those registered with
/// GEOZONE_TEST, or the one named by the first argument, like "GeoZoneCoreTest GeoZoneParserTest -v2". The other
/// arguments are those of QTest.
/// @return 0 if all tests passed, 1 if some failed, 2 for an unknown test
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    // Settings the core may write stay apart from those of QGroundControl
    QCoreApplication::setOrganizationName(QStringLiteral("QGroundControl"));
    QCoreApplication::setApplicationName(QStringLiteral("GeoZoneCoreTest"));

    const GeoZoneCoreTests &tests = geoZoneCoreTests();
    QStringList arguments = QCoreApplication::arguments();
    QString selected;
    if ((arguments.count() > 1) && !arguments[1].startsWith(QLatin1Char('-'))) {
        selected = arguments.takeAt(1);
        if (!tests.contains(selected)) {
            std::fprintf(stderr, "Unknown test %s, the tests are:\n", qPrintable(selected));
            for (const QString &name : tests.keys()) {
                std::fprintf(stderr, "  %s\n", qPrintable(name));
            }
            return 2;
        }
    }

    int failures = 0;
    for (auto it = tests.cbegin(); it != tests.cend(); ++it) {
        if (selected.isEmpty() || (it.key() == selected)) {
            const std::unique_ptr<QObject> object = it.value()();
            failures += QTest::qExec(object.get(), arguments);
        }
    }
    // Not the number of failures, which could wrap around to 0 as an exit status
    return (failures == 0) ? 0 : 1;
}
