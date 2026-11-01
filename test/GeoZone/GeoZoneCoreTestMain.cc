/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneCoreTests.h"

#include <QtCore/QCoreApplication>
#include <QtTest/QTest>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>

/// Runs the geozone tests which only need the GeoZoneCore library, without QGroundControl: those of the lists of
/// CMakeLists.txt, or the one named by the first argument, like "GeoZoneCoreTest GeoZoneParserTest -v2". The other
/// arguments are those of QTest.
/// @return 0 if all tests passed, 1 if some failed, 2 for an unknown test
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    // Settings the core may write stay apart from those of QGroundControl
    QCoreApplication::setOrganizationName(QStringLiteral("QGroundControl"));
    QCoreApplication::setApplicationName(QStringLiteral("GeoZoneCoreTest"));

    struct Test {
        QString name;
        std::function<std::unique_ptr<QObject>()> create;
    };
    const QList<Test> tests = {
#define GEOZONE_CORE_TEST_ENTRY(Test) { QStringLiteral(#Test), []() -> std::unique_ptr<QObject> { return std::make_unique<Test>(); } },
        GEOZONE_CORE_TESTS(GEOZONE_CORE_TEST_ENTRY)
#undef GEOZONE_CORE_TEST_ENTRY
    };

    QStringList arguments = QCoreApplication::arguments();
    QString selected;
    if ((arguments.count() > 1) && !arguments[1].startsWith(QLatin1Char('-'))) {
        selected = arguments.takeAt(1);
        if (std::none_of(tests.cbegin(), tests.cend(), [&selected](const Test &test) { return test.name == selected; })) {
            std::fprintf(stderr, "Unknown test %s, the tests are:\n", qPrintable(selected));
            for (const Test &test : tests) {
                std::fprintf(stderr, "  %s\n", qPrintable(test.name));
            }
            return 2;
        }
    }

    int failures = 0;
    for (const Test &test : tests) {
        if (selected.isEmpty() || (test.name == selected)) {
            const std::unique_ptr<QObject> object = test.create();
            failures += QTest::qExec(object.get(), arguments);
        }
    }
    // Not the number of failures, which could wrap around to 0 as an exit status
    return (failures == 0) ? 0 : 1;
}
