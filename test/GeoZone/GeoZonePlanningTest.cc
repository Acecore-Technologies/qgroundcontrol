/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZonePlanningTest.h"
#include "GeoZoneBriefing.h"
#include "GeoZoneBriefingExport.h"
#include "GeoZoneFenceTags.h"
#include "GeoZoneMissionReader.h"
#include "GeoZoneMissionWorker.h"
#include "GeoZoneOperatorZones.h"
#include "GeoZonePlanAudit.h"
#include "GeoZoneSourceRepository.h"
#include "GeoZoneTestBase.h"
#include "GeoZoneTestHelpers.h"
#include "MultiVehicleManager.h"
#include "Vehicle.h"

#include <QtCore/QHash>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QScopeGuard>
#include <QtCore/QStandardPaths>
#include <QtGui/QFont>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QPalette>

#include <algorithm>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

using namespace GeoZoneTestHelpers;

namespace {

/// Glyphs of the fonts of a PDF document written by Qt that the ToUnicode maps of the fonts don't map back to letters, so
/// that text extraction loses them or gives ligatures like U+FB01 instead of "fi". Qt numbers the glyphs of each font in
/// the order the document uses them, so the glyphs without characters are the gaps below the highest one mapped.
/// @param fontCount Fonts with a ToUnicode map
QStringList unmappedPdfGlyphs(const QByteArray &pdf, int &fontCount)
{
    static const QRegularExpression rangeExpression(QStringLiteral(R"(<([0-9A-Fa-f]+)>\s*<([0-9A-Fa-f]+)>\s*(\[[^\]]*\]|<[0-9A-Fa-f]+>))"));
    static const QRegularExpression charExpression(QStringLiteral(R"(<([0-9A-Fa-f]+)>\s*<([0-9A-Fa-f]+)>)"));
    static const QRegularExpression codeExpression(QStringLiteral(R"(<([0-9A-Fa-f]+)>)"));
    const auto sections = [](const QString &cmap, const QString &begin, const QString &end) {
        QStringList result;
        qsizetype from = 0;
        while ((from = cmap.indexOf(begin, from)) >= 0) {
            const qsizetype to = cmap.indexOf(end, from);
            if (to < 0) {
                break;
            }
            result.append(cmap.mid(from + begin.size(), to - from - begin.size()));
            from = to;
        }
        return result;
    };

    QStringList unmapped;
    fontCount = 0;
    qsizetype from = 0;
    while ((from = pdf.indexOf("begincmap", from)) >= 0) {
        const qsizetype to = pdf.indexOf("endcmap", from);
        if (to < 0) {
            break;
        }
        const QString cmap = QString::fromLatin1(pdf.mid(from, to - from));
        from = to;
        // Characters of the glyphs, 0 for none
        QHash<uint, uint> characters;
        for (const QString &section : sections(cmap, QStringLiteral("beginbfrange"), QStringLiteral("endbfrange"))) {
            for (const QRegularExpressionMatch &match : rangeExpression.globalMatch(section)) {
                const uint first = match.captured(1).toUInt(nullptr, 16);
                const uint last = match.captured(2).toUInt(nullptr, 16);
                const QString target = match.captured(3);
                if (target.startsWith(QLatin1Char('['))) {
                    uint code = first;
                    for (const QRegularExpressionMatch &character : codeExpression.globalMatch(target)) {
                        characters.insert(code++, character.captured(1).toUInt(nullptr, 16));
                    }
                } else {
                    const uint character = codeExpression.match(target).captured(1).toUInt(nullptr, 16);
                    for (uint code = first; code <= last; code++) {
                        characters.insert(code, (character == 0) ? 0 : (character + code - first));
                    }
                }
            }
        }
        for (const QString &section : sections(cmap, QStringLiteral("beginbfchar"), QStringLiteral("endbfchar"))) {
            for (const QRegularExpressionMatch &match : charExpression.globalMatch(section)) {
                characters.insert(match.captured(1).toUInt(nullptr, 16), match.captured(2).toUInt(nullptr, 16));
            }
        }
        uint highest = 0;
        for (auto it = characters.cbegin(); it != characters.cend(); ++it) {
            highest = (it.value() != 0) ? (std::max)(highest, it.key()) : highest;
        }
        for (uint code = 1; code <= highest; code++) {
            const uint character = characters.value(code);
            // Latin ligatures of the Alphabetic Presentation Forms
            const bool ligature = (character >= 0xFB00) && (character <= 0xFB06);
            if ((character == 0) || ligature) {
                unmapped.append(QStringLiteral("font %1 glyph %2 to U+%3").arg(fontCount).arg(code).arg(character, 4, 16, QLatin1Char('0')));
            }
        }
        fontCount++;
    }
    return unmapped;
}

} // namespace

GeoZonePlanningTest::GeoZonePlanningTest() = default;

GeoZonePlanningTest::~GeoZonePlanningTest() = default;

void GeoZonePlanningTest::initTestCase()
{
    _metricUnits = std::make_unique<ScopedMetricUnits>();
}

void GeoZonePlanningTest::cleanupTestCase()
{
    _metricUnits.reset();
}

void GeoZonePlanningTest::_testGuidedPathWarning()
{
    const QGeoCoordinate center(47, 8);
    const QGeoCoordinate square = center.atDistanceAndAzimuth(3000, 90);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _polygonFeature(QStringLiteral("B"), QStringLiteral("CONDITIONAL"), { square.atDistanceAndAzimuth(282.8, 45), square.atDistanceAndAzimuth(282.8, 135),
                                                                              square.atDistanceAndAzimuth(282.8, 225), square.atDistanceAndAzimuth(282.8, 315) }),
        _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(1000, 90), 200),
        // Unpublished restriction types can't be assumed harmless
        _circleFeature(QStringLiteral("X"), QStringLiteral("MYSTERY"), center.atDistanceAndAzimuth(2000, 90), 100),
        _circleFeature(QStringLiteral("N"), QStringLiteral("NO_RESTRICTION"), center.atDistanceAndAzimuth(500, 90), 100),
    }), errorString), qPrintable(errorString));

    // Restricted zones and unknown restrictions are warned about, others are information
    const QList<GeoZone::Position> east = { GeoZone::Position{ center }, GeoZone::Position{ center.atDistanceAndAzimuth(2000, 90) },
                                            GeoZone::Position{ center.atDistanceAndAzimuth(4000, 90) }, GeoZone::Position{ center } };
    GeoZoneMonitor monitor(&manager);
    const QString warning = monitor.guidedCheck()->pathWarning(east);
    QVERIFY(warning.contains(QStringLiteral("A (Prohibited)")));
    QVERIFY(warning.contains(QStringLiteral("X (Unknown)")));
    QVERIFY(!warning.contains(QStringLiteral("N (")));
}

void GeoZonePlanningTest::_testApproximationReports()
{
    // East through a zone above a flight level, one with limits in an unknown reference, and one active from an unknown
    // daylight event to sunset, see GeoZoneEvaluatorTest::_testApproximations
    const QGeoCoordinate start(47, 8);
    QJsonObject scheduled = _circleFeature(QStringLiteral("SCHEDULED"), QStringLiteral("PROHIBITED"), start.atDistanceAndAzimuth(3000, 90), 200);
    scheduled[QStringLiteral("applicability")] = QJsonArray{ QJsonObject{ { "permanent", "NO" }, { "schedule", QJsonArray{ QJsonObject{
        { "day", QJsonArray{ "ANY" } }, { "startEvent", "MOONRISE" }, { "endEvent", "SS" } } } } } };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _withLimits(_circleFeature(QStringLiteral("LEVEL"), QStringLiteral("PROHIBITED"), start.atDistanceAndAzimuth(1000, 90), 200), 0, QStringLiteral("STD"), 3000, QStringLiteral("STD")),
        _withLimits(_circleFeature(QStringLiteral("UNKNOWN"), QStringLiteral("PROHIBITED"), start.atDistanceAndAzimuth(2000, 90), 200), 100, QStringLiteral("XYZ"), 200, QStringLiteral("XYZ")),
        scheduled,
    }), errorString), qPrintable(errorString));
    const auto zoneNamed = [&manager](const QString &identifier) {
        const auto it = std::find_if(manager.zoneList().cbegin(), manager.zoneList().cend(), [&identifier](const GeoZone *zone) { return zone->identifier() == identifier; });
        return (it == manager.zoneList().cend()) ? nullptr : *it;
    };

    // The briefing documents have them per zone and overall, in the JSON and with a note in the web page
    const QDateTime flightStart = _utc(QDate(2027, 6, 1), QTime(10, 0));
    const QDateTime flightEnd = flightStart.addSecs(600);
    GeoZonePathEvaluator::Path path = _evaluatorPath({ start, start.atDistanceAndAzimuth(4000, 90) }, { 500, 500 });
    path.setTimes(flightStart, flightEnd);
    GeoZoneBriefing::Input input;
    input.mission.name = QStringLiteral("Approximate");
    input.paths = { path };
    input.start = flightStart;
    input.end = flightEnd;
    const GeoZoneBriefing briefing = GeoZoneBriefing::build(manager.catalog(), input);
    const QJsonObject json = GeoZoneBriefingExport::toJson(briefing);
    QCOMPARE(json[QStringLiteral("approximations")].toArray().count(), 3);
    const QJsonObject levelJson = json[QStringLiteral("zones")].toArray()[0].toObject();
    QCOMPARE(levelJson[QStringLiteral("identifier")].toString(), QStringLiteral("LEVEL"));
    QCOMPARE(levelJson[QStringLiteral("approximations")].toArray(), QJsonArray({ QStringLiteral("flight_level") }));
    const QString html = GeoZoneBriefingExport::toHtml(briefing);
    QVERIFY(html.contains(QStringLiteral("Some results are approximate")));
    QVERIFY(html.contains(QStringLiteral("Flight level limits converted with the QNH setting")));

    // The mission check notes them in its warning
    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    (void) missionController->insertSimpleMissionItem(start.atDistanceAndAzimuth(500, 270), 1);
    (void) missionController->insertSimpleMissionItem(start.atDistanceAndAzimuth(1500, 90), 2);
    QTRY_VERIFY(check.zones(GeoZoneMissionCheck::Crossed).contains(zoneNamed(QStringLiteral("LEVEL"))));
    QVERIFY(check.entry(zoneNamed(QStringLiteral("LEVEL")))->approximations.testFlag(GeoZonePathEvaluator::FlightLevel));
    QVERIFY2(check.warningText().contains(QStringLiteral("Approximate: ")), qPrintable(check.warningText()));
    QVERIFY(check.warningText().contains(QStringLiteral("Flight level limits converted with the QNH setting")));
}

void GeoZonePlanningTest::_testAntimeridianProfile()
{
    // A zone from 179.9 E to 179.9 W, see GeoZoneEvaluatorTest::_testAntimeridianQueries
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ZoneCatalog zones(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(zones.loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _polygonFeature(QStringLiteral("X"), QStringLiteral("PROHIBITED"), { QGeoCoordinate(-16.6, 179.9), QGeoCoordinate(-16.4, 179.9),
                                                                             QGeoCoordinate(-16.4, -179.9), QGeoCoordinate(-16.6, -179.9) }),
    }), errorString), qPrintable(errorString));

    // The profile of a segment across the antimeridian passes through the zone
    const QGeoCoordinate from(-16.5, 179.8);
    const QGeoCoordinate to(-16.5, -179.8);
    const double entryDistance = from.distanceTo(QGeoCoordinate(-16.5, 179.9));
    GeoZoneMissionPath::Segment segment;
    segment.from = from;
    segment.to = to;
    segment.distance = from.distanceTo(to);
    const QList<GeoZoneProfile::Band> bands = GeoZoneProfile::computeBands({ segment }, zones.catalog());
    QCOMPARE(bands.count(), 1);
    QCOMPARE(bands[0].name, QStringLiteral("X"));
    QVERIFY(qAbs(bands[0].points.constFirst().distance - entryDistance) < 20);
    QVERIFY(qAbs(bands[0].points.constLast().distance - (segment.distance - entryDistance)) < 20);
}

void GeoZonePlanningTest::_testMissionCheck()
{
    // Before the temporary restriction of 2027
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2026, 6, 15), QTime(10, 0)));
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString fileName = _writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300),
        _circleFeature(QStringLiteral("C"), QStringLiteral("CONDITIONAL"), center.atDistanceAndAzimuth(2000, 0), 300),
        _circleFeature(QStringLiteral("N"), QStringLiteral("NO_RESTRICTION"), center.atDistanceAndAzimuth(2000, 180), 300),
        // On the leg from 1000 m east to 2000 m north
        _circleFeature(QStringLiteral("U"), QStringLiteral("USPACE"), center.atDistanceAndAzimuth(1118, 26.57), 200),
    });
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(fileName, errorString), qPrintable(errorString));

    // Far above the planned altitudes
    QJsonObject high = _circleFeature(QStringLiteral("HIGH"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(500, 90), 300);
    QJsonArray geometry = high[QStringLiteral("geometry")].toArray();
    QJsonObject layer = geometry[0].toObject();
    layer[QStringLiteral("lowerLimit")] = 5000;
    layer[QStringLiteral("lowerVerticalReference")] = QStringLiteral("AMSL");
    layer[QStringLiteral("upperLimit")] = 6000;
    layer[QStringLiteral("upperVerticalReference")] = QStringLiteral("AMSL");
    geometry[0] = layer;
    high[QStringLiteral("geometry")] = geometry;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("high.json"), QJsonArray{ high }), errorString), qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();

    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);

    // Passes the no restriction zone only, which is reported without a warning
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(2000, 180).atDistanceAndAzimuth(1000, 270), 1);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(2000, 180).atDistanceAndAzimuth(1000, 90), 2);
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Permitted).count(), 1);
    QVERIFY(check.zones(GeoZoneMissionCheck::Crossed).isEmpty());
    QVERIFY(check.warningText().isEmpty());
    QVERIFY(check.zones(GeoZoneMissionCheck::USpace).isEmpty());
    QVERIFY(check.infoText().startsWith(QStringLiteral("Areas without restriction: N.")));

    // Through the prohibited zone and then the conditional one, listed most restrictive first
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 270), 3);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 90), 4);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(2000, 0), 5);
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 2);
    QCOMPARE(check.zones(GeoZoneMissionCheck::Crossed)[0]->identifier(), QStringLiteral("P"));
    QCOMPARE(check.zones(GeoZoneMissionCheck::Crossed)[1]->identifier(), QStringLiteral("C"));
    QVERIFY(check.warningText().contains(QStringLiteral("P (Prohibited)")));
    // The path north also passes through the U-space airspace
    QCOMPARE(check.zones(GeoZoneMissionCheck::USpace).count(), 1);
    QVERIFY(check.infoText().contains(QStringLiteral("U-space airspace: U.")));
    QVERIFY(check.crossedZonesBoundingBox().contains(center));
    QVERIFY(check.crossedZonesBoundingBox().contains(center.atDistanceAndAzimuth(2000, 0)));

    // The fence must exclude the prohibited zones, circles and polygons added from them do
    GeoFenceController *const geoFenceController = masterController.geoFenceController();
    QVERIFY(geoFenceController->supported());
    check.setGeoFenceController(geoFenceController);
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Unfenced).count(), 1);
    QVERIFY(check.fenceText().contains(QStringLiteral("P")));
    geoFenceController->addInclusionCircle(center.atDistanceAndAzimuth(1000, 315), center.atDistanceAndAzimuth(1000, 135));
    check.update();
    QCOMPARE(check.zones(GeoZoneMissionCheck::Unfenced).count(), 1);
    GeoZoneFenceImporter importer;
    importer.setGeoFenceController(geoFenceController);
    QCOMPARE(importer.addZones(GeoZoneInfo::toVariantList(check.zones(GeoZoneMissionCheck::Unfenced))), 1);
    QTRY_VERIFY(check.zones(GeoZoneMissionCheck::Unfenced).isEmpty());
    QVERIFY(check.fenceText().isEmpty());
    GeoZoneFenceTags::deleteFences(geoFenceController, false);
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Unfenced).count(), 1);
    // A polygon zone excluded by a simplified polygon fence
    const QList<QGeoCoordinate> squareZone = { center.atDistanceAndAzimuth(300, 315), center.atDistanceAndAzimuth(300, 45),
                                               center.atDistanceAndAzimuth(300, 135), center.atDistanceAndAzimuth(300, 225) };
    GeoZone::Data squareData;
    squareData.restriction = GeoZone::Prohibited;
    GeoZone::Volume squareVolume;
    squareVolume.rings = { GeoZone::toRing(squareZone) };
    squareData.volumes = { squareVolume };
    GeoZone square(squareData);
    QVERIFY(!GeoZoneMissionCheck::fenceExcludes(geoFenceController, &square));
    QCOMPARE(importer.addExclusions({ &square }, 25), 1);
    QVERIFY(GeoZoneMissionCheck::fenceExcludes(geoFenceController, &square));
    GeoZoneFenceTags::deleteFences(geoFenceController, false);
    check.setGeoFenceController(nullptr);

    // A temporary restriction on the path counts at any time, or when it is active during the planned flight
    QJsonObject temporary = _circleFeature(QStringLiteral("T"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(700, 270), 100);
    temporary[QStringLiteral("applicability")] = QJsonArray{ QJsonObject{ { "permanent", "NO" }, { "startDateTime", "2027-01-01T10:00:00Z" }, { "endDateTime", "2027-01-01T12:00:00Z" } } };
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("temporary.json"), QJsonArray{ temporary }), errorString), qPrintable(errorString));
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 3);
    QCOMPARE(check.zones(GeoZoneMissionCheck::Inactive).count(), 0);
    manager.planning()->setPlannedStart(_utc(QDate(2027, 1, 1), QTime(8, 0)));
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 2);
    QCOMPARE(check.zones(GeoZoneMissionCheck::Inactive).count(), 1);
    QVERIFY(check.infoText().contains(QStringLiteral("1 zone(s) on the path are not active at the planned flight time.")));
    // The mission takes a few minutes, so a start just before the restriction overlaps it
    const QDateTime overlappingStart = _utc(QDate(2027, 1, 1), QTime(10, 0)).addSecs(-std::llround(missionController->missionTime() / 2));
    QVERIFY(missionController->missionTime() > 10);
    manager.planning()->setPlannedStart(overlappingStart);
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 3);
    // A mission started now, like one on the vehicle, ignores the planned start
    const GeoZoneMonitor monitor(&manager);
    const QString startWarning = monitor.guidedCheck()->startWarning(missionController);
    QVERIFY2(startWarning.contains(QStringLiteral("P (Prohibited)")) && !startWarning.contains(QStringLiteral("T (Prohibited)")), qPrintable(startWarning));
    manager.planning()->clearPlannedStart();
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Inactive).count(), 0);
    QCOMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 3);

    // A survey whose area covers a zone its flight lines miss
    const QGeoCoordinate surveyCenter = center.atDistanceAndAzimuth(5000, 180);
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("survey.json"),
        QJsonArray{ _circleFeature(QStringLiteral("S"), QStringLiteral("PROHIBITED"), surveyCenter.atDistanceAndAzimuth(395, 45), 2) }), errorString), qPrintable(errorString));
    const int surveyIndex = missionController->visualItems()->count();
    SurveyComplexItem *const survey = qobject_cast<SurveyComplexItem*>(missionController->insertComplexMissionItem(SurveyComplexItem::name, surveyCenter, surveyIndex));
    QVERIFY(survey);
    survey->surveyAreaPolygon()->clear();
    survey->surveyAreaPolygon()->appendVertices({ surveyCenter.atDistanceAndAzimuth(424, 315), surveyCenter.atDistanceAndAzimuth(424, 45),
                                                  surveyCenter.atDistanceAndAzimuth(424, 135), surveyCenter.atDistanceAndAzimuth(424, 225) });
    const auto surveyZoneCrossed = [&check]() {
        const QList<const GeoZone*> crossed = check.zones(GeoZoneMissionCheck::Crossed);
        return std::any_of(crossed.cbegin(), crossed.cend(), [](const GeoZone *zone) {
            return zone->identifier() == QStringLiteral("S");
        });
    };
    QTRY_VERIFY(surveyZoneCrossed());

    // Zones are released before the manager deletes them
    manager.repository()->clear();
    QVERIFY(check.zones(GeoZoneMissionCheck::Crossed).isEmpty());
}

void GeoZonePlanningTest::_testMissionControllerSwitch()
{
    const ScopedEnvironment environment;
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300),
    }), errorString), qPrintable(errorString));

    // A mission through the zone, and an empty one
    PlanMasterController crossing;
    crossing.setFlyView(false);
    crossing.start();
    (void) crossing.missionController()->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 270), 1);
    (void) crossing.missionController()->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 90), 2);
    QTRY_VERIFY(!GeoZoneMissionReader::path(crossing.missionController()).paths().isEmpty());
    PlanMasterController empty;
    empty.setFlyView(false);
    empty.start();

    GeoZoneMissionCheck check(&manager);
    GeoZoneProfile profile(&manager);
    GeoZoneMissionWorker *const checkWorker = check.findChild<GeoZoneMissionWorker*>();
    GeoZoneMissionWorker *const profileWorker = profile.findChild<GeoZoneMissionWorker*>();
    QVERIFY(checkWorker && profileWorker);
    // Evaluations only start when triggered here
    checkWorker->setDelay(600000);
    profileWorker->setDelay(600000);
    check.setMissionController(crossing.missionController());
    profile.setMissionController(crossing.missionController());
    checkWorker->trigger();
    profileWorker->trigger();
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 1);
    QTRY_COMPARE(profile.bands().count(), 1);

    // The results of the previous mission are cleared, and those of its evaluations in flight never applied
    checkWorker->trigger();
    profileWorker->trigger();
    QVERIFY(checkWorker->busy());
    QSignalSpy crossedSpy(&check, &GeoZoneMissionCheck::zonesChanged);
    QSignalSpy bandsSpy(&profile, &GeoZoneProfile::bandsChanged);
    check.setMissionController(empty.missionController());
    profile.setMissionController(empty.missionController());
    QVERIFY(check.zones(GeoZoneMissionCheck::Crossed).isEmpty());
    QVERIFY(check.relevantZones().isEmpty());
    QVERIFY(profile.bands().isEmpty());
    QCOMPARE(crossedSpy.count(), 1);
    QCOMPARE(bandsSpy.count(), 1);
    QVERIFY(_settleWorkers());
    QVERIFY(check.zones(GeoZoneMissionCheck::Crossed).isEmpty());
    QVERIFY(profile.bands().isEmpty());
    QCOMPARE(crossedSpy.count(), 1);
    QCOMPARE(bandsSpy.count(), 1);

    // Then the new mission is checked
    checkWorker->trigger();
    QVERIFY(checkWorker->busy());
    QTRY_VERIFY(!checkWorker->busy());
    QVERIFY(check.zones(GeoZoneMissionCheck::Crossed).isEmpty());
    check.setMissionController(crossing.missionController());
    checkWorker->trigger();
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 1);
}

void GeoZonePlanningTest::_testAuthorisations()
{
    // Authorisations are for local days, the test stays within one
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2026, 6, 15), QTime(12, 0)));
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString storage = dir.filePath(QStringLiteral("storage"));
    GeoZoneManager manager(storage);
    QString errorString;
    QJsonObject controlZone = _circleFeature(QStringLiteral("CTR"), QStringLiteral("REQ_AUTHORISATION"), center, 300);
    controlZone[QStringLiteral("country")] = QStringLiteral("CHE");
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ controlZone }), errorString), qPrintable(errorString));
    GeoZone *const zone = manager.zoneList().constFirst();
    GeoZoneAuthorisations *const authorisations = manager.planning()->authorisations();
    const QVariant zoneInfo = QVariant::fromValue(GeoZoneInfo(zone));
    QCOMPARE(GeoZoneAuthorisations::zoneKey(zone), QStringLiteral("CHE/CTR"));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 270), 1);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 90), 2);
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 1);

    // Dates are local days, the last one included
    QVERIFY(!authorisations->authorise(zoneInfo, QString(), QStringLiteral("2026-13-40"), QString()).isEmpty());
    QVERIFY(!authorisations->authorise(zoneInfo, QString(), QStringLiteral("2026-11-02"), QStringLiteral("2026-11-01")).isEmpty());
    QCOMPARE(authorisations->count(), 0);

    // An authorisation without end is listed for information instead of warned about
    const QDate today = environment.now().toLocalTime().date();
    QVERIFY(authorisations->authorise(zoneInfo, QStringLiteral(" P-123 "), QString(), QString()).isEmpty());
    QCOMPARE(authorisations->count(), 1);
    QCOMPARE(authorisations->authorisationOf(zoneInfo)[QStringLiteral("permit")].toString(), QStringLiteral("P-123"));
    QCOMPARE(authorisations->authorisationOf(zoneInfo)[QStringLiteral("statusText")].toString(), QStringLiteral("Valid"));
    QTRY_VERIFY(check.zones(GeoZoneMissionCheck::Crossed).isEmpty());
    QCOMPARE(check.zones(GeoZoneMissionCheck::Authorised).count(), 1);
    QVERIFY(check.infoText().contains(QStringLiteral("Authorised: CTR.")));

    // Alerts and guided commands leave the zone out while authorised
    GeoZoneMonitor monitor(&manager);
    QSignalSpy alertSpy(&monitor, &GeoZoneMonitor::alert);
    monitor.evaluate({ center });
    QCOMPARE(alertSpy.count(), 0);
    const QList<GeoZone::Position> path = { { center.atDistanceAndAzimuth(1000, 270) }, { center.atDistanceAndAzimuth(1000, 90) } };
    QVERIFY(monitor.guidedCheck()->pathWarning(path).isEmpty());

    // Expired authorisations don't count
    QVERIFY(authorisations->authorise(zoneInfo, QStringLiteral("P-122"), QString(), today.addDays(-1).toString(Qt::ISODate)).isEmpty());
    QCOMPARE(authorisations->count(), 1);
    QVERIFY(authorisations->authorisationOf(zoneInfo)[QStringLiteral("statusText")].toString().startsWith(QStringLiteral("Expired")));
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 1);
    QVERIFY(!monitor.guidedCheck()->pathWarning(path).isEmpty());
    monitor.evaluate({ center });
    QCOMPARE(alertSpy.count(), 1);
    QVERIFY2(check.warningText().contains(QStringLiteral("CTR: authorised until %1.").arg(today.addDays(-1).toString(Qt::ISODate))), qPrintable(check.warningText()));

    // A permit from tomorrow doesn't cover a mission without planned start, which may be flown now
    QVERIFY(authorisations->authorise(zoneInfo, QStringLiteral("P-124"), today.addDays(1).toString(Qt::ISODate), today.addDays(3).toString(Qt::ISODate)).isEmpty());
    QTRY_VERIFY(check.warningText().contains(QStringLiteral("CTR: authorised from %1.").arg(today.addDays(1).toString(Qt::ISODate))));
    QCOMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 1);
    QVERIFY(check.zones(GeoZoneMissionCheck::Authorised).isEmpty());

    // An authorisation from tomorrow covers a flight planned then, not one now
    QVERIFY(authorisations->authorise(zoneInfo, QStringLiteral("P-124"), today.addDays(1).toString(Qt::ISODate), today.addDays(3).toString(Qt::ISODate)).isEmpty());
    manager.planning()->setPlannedStart(QDateTime(today, QTime(12, 0)));
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 1);
    manager.planning()->setPlannedStart(QDateTime(today.addDays(2), QTime(12, 0)));
    QTRY_VERIFY(check.zones(GeoZoneMissionCheck::Crossed).isEmpty());

    // Plans entering the zone keep the planned start and the authorisation
    QJsonObject planJson;
    GeoZonePlanData::save(&masterController, planJson, &manager);
    const QJsonObject geoZones = planJson[QString(GeoZonePlanData::jsonKey)].toObject();
    QCOMPARE(QDateTime::fromString(geoZones[QStringLiteral("plannedStart")].toString(), Qt::ISODate), QDateTime(today.addDays(2), QTime(12, 0)).toUTC());
    QCOMPARE(geoZones[QStringLiteral("authorisations")].toArray().count(), 1);
    QCOMPARE(geoZones[QStringLiteral("authorisations")].toArray()[0].toObject()[QStringLiteral("permit")].toString(), QStringLiteral("P-124"));

    // Opening the plan offers its authorisations, which are only added when accepted
    authorisations->revoke(QStringLiteral("CHE/CTR"));
    manager.planning()->clearPlannedStart();
    QCOMPARE(authorisations->count(), 0);
    GeoZonePlanData::load(nullptr, planJson, &manager);
    QCOMPARE(manager.planning()->plannedStart(), QDateTime(today.addDays(2), QTime(12, 0)).toUTC());
    QCOMPARE(authorisations->count(), 0);
    QCOMPARE(authorisations->pendingAuthorisations().count(), 1);
    QCOMPARE(authorisations->pendingAuthorisations()[0].toMap()[QStringLiteral("permit")].toString(), QStringLiteral("P-124"));
    authorisations->dismissPending();
    QCOMPARE(authorisations->count(), 0);
    QVERIFY(authorisations->pendingAuthorisations().isEmpty());
    GeoZonePlanData::load(nullptr, planJson, &manager);
    authorisations->acceptPending();
    QCOMPARE(authorisations->count(), 1);
    QVERIFY(authorisations->pendingAuthorisations().isEmpty());
    // Authorisations already known aren't offered again
    GeoZonePlanData::load(nullptr, planJson, &manager);
    QVERIFY(authorisations->pendingAuthorisations().isEmpty());

    // Kept across restarts next to the stored sources
    GeoZoneManager restarted(storage);
    restarted.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
    QCOMPARE(restarted.zoneCount(), 1);
    QCOMPARE(restarted.planning()->authorisations()->count(), 1);
    QVERIFY(restarted.planning()->authorisations()->find(restarted.zoneList().constFirst()));
    manager.planning()->clearPlannedStart();

    // Authorisations limited to a ceiling or an area cover the flight staying within them
    QVERIFY(!authorisations->authorise(zoneInfo, QStringLiteral("P-125"), QString(), QString(), { { QStringLiteral("ceiling"), QStringLiteral("high") } }).isEmpty());
    QVERIFY(!authorisations->authorise(zoneInfo, QStringLiteral("P-125"), QString(), QString(), { { QStringLiteral("dailyFrom"), QStringLiteral("08:00") } }).isEmpty());
    QVERIFY(authorisations->authorise(zoneInfo, QStringLiteral("P-125"), QString(), QString(), { { QStringLiteral("dailyFrom"), QStringLiteral("0000") },
                                                                                                { QStringLiteral("dailyTo"), QStringLiteral("0:00") } }).isEmpty());
    QCOMPARE(authorisations->authorisationOf(zoneInfo)[QStringLiteral("dailyTo")].toString(), QStringLiteral("00:00"));
    QVERIFY(authorisations->authorise(zoneInfo, QStringLiteral("P-125"), QString(), QString(), { { QStringLiteral("ceiling"), QStringLiteral("1") } }).isEmpty());
    QCOMPARE(authorisations->authorisationOf(zoneInfo)[QStringLiteral("ceiling")].toString(), QStringLiteral("1"));
    QCOMPARE(authorisations->ceilingUnits(zoneInfo), QStringLiteral("m AMSL"));
    QTRY_VERIFY2(check.warningText().contains(QStringLiteral("CTR: authorised up to 1 m AMSL")), qPrintable(check.warningText()));
    QCOMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 1);
    QVERIFY(manager.planning()->missionArea().isValid());
    QVERIFY(authorisations->authorise(zoneInfo, QStringLiteral("P-125"), QString(), QString(), {
        { QStringLiteral("limitToMission"), true }, { QStringLiteral("missionArea"), QVariant::fromValue(manager.planning()->missionArea()) },
        { QStringLiteral("aircraft"), QStringLiteral(" HB-ABC ") } }).isEmpty());
    QVERIFY(authorisations->authorisationOf(zoneInfo)[QStringLiteral("limitToMission")].toBool());
    QVERIFY2(authorisations->authorisationOf(zoneInfo)[QStringLiteral("statusText")].toString().endsWith(QStringLiteral("Valid, within its area, aircraft HB-ABC")),
             qPrintable(authorisations->authorisationOf(zoneInfo)[QStringLiteral("statusText")].toString()));
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Authorised).count(), 1);
    QVERIFY(check.zones(GeoZoneMissionCheck::Crossed).isEmpty());
    // The check on this thread, like before an upload, checks the flight against the scope of the authorisation too,
    // so that the zone isn't reported as crossed and the plan keeps the authorisation
    check.update();
    QCOMPARE(check.zones(GeoZoneMissionCheck::Authorised).count(), 1);
    QVERIFY2(check.zones(GeoZoneMissionCheck::Crossed).isEmpty(), qPrintable(check.warningText()));
    QVERIFY(check.settled());
    planJson = QJsonObject();
    GeoZonePlanData::save(&masterController, planJson, &manager);
    QCOMPARE(planJson[QString(GeoZonePlanData::jsonKey)].toObject()[QStringLiteral("authorisations")].toArray()[0].toObject()[QStringLiteral("area")].toArray().count(), 4);
}

void GeoZonePlanningTest::_testNotifications()
{
    // A conditional zone whose authority is notified two days before flights
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2026, 6, 15), QTime(12, 0)));
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QJsonObject zoneJson = _circleFeature(QStringLiteral("NOTE"), QStringLiteral("CONDITIONAL"), center, 300);
    zoneJson[QStringLiteral("country")] = QStringLiteral("CHE");
    zoneJson[QStringLiteral("zoneAuthority")] = QJsonArray{ QJsonObject{ { QStringLiteral("name"), QStringLiteral("Tower") }, { QStringLiteral("purpose"), QStringLiteral("NOTIFICATION") },
                                                                         { QStringLiteral("intervalBefore"), QStringLiteral("P2D") } } };
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{ zoneJson }), errorString), qPrintable(errorString));
    const GeoZone *const zone = manager.zoneList().constFirst();
    QCOMPARE(zone->data().notificationLeadSecs, qint64{ 2 * 24 * 3600 });
    const QVariant zoneInfo = QVariant::fromValue(GeoZoneInfo(zone));
    GeoZoneAuthorisations *const authorisations = manager.planning()->authorisations();
    QCOMPARE(authorisations->notificationRequirement(zoneInfo), QStringLiteral("Notify Tower 2 d before the flight"));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 270), 1);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 90), 2);
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    QTRY_VERIFY2(check.infoText().contains(QStringLiteral("NOTE: notify Tower 2 d before the flight.")), qPrintable(check.infoText()));

    // With a planned start, by when, and a warning once the notice period has passed
    const QDateTime start(QDate(2026, 6, 20), QTime(10, 0));
    manager.planning()->setPlannedStart(start);
    const QString due = QLocale().toString(start.addDays(-2), QStringLiteral("yyyy-MM-dd HH:mm"));
    QTRY_VERIFY2(check.infoText().contains(QStringLiteral("NOTE: notify Tower by %1.").arg(due)), qPrintable(check.infoText()));
    manager.planning()->setPlannedStart(QDateTime(QDate(2026, 6, 16), QTime(10, 0)));
    QTRY_VERIFY2(check.warningText().contains(QStringLiteral("NOTE: notify Tower by")) && check.warningText().contains(QStringLiteral("the notice period has passed.")),
                 qPrintable(check.warningText()));

    // The briefing says so too
    GeoZoneBriefingExport exporter(&manager);
    exporter.setMissionController(missionController);
    const GeoZoneBriefing briefing = exporter.briefing();
    QCOMPARE(briefing.zones.count(), 1);
    QVERIFY2(briefing.zones[0].notification.endsWith(QStringLiteral("the notice period has passed.")), qPrintable(briefing.zones[0].notification));
    QCOMPARE(GeoZoneBriefingExport::toJson(briefing)[QStringLiteral("zones")].toArray()[0].toObject()[QStringLiteral("notification")].toString(),
             briefing.zones[0].notification);

    // The notification recorded satisfies it, without authorising the flight
    QVERIFY(authorisations->recordNotification(zoneInfo, QStringLiteral(" N-1 "), QString(), QString()).isEmpty());
    QCOMPARE(authorisations->notificationOf(zoneInfo)[QStringLiteral("permit")].toString(), QStringLiteral("N-1"));
    QCOMPARE(authorisations->notificationOf(zoneInfo)[QStringLiteral("statusText")].toString(), QStringLiteral("Notified, valid"));
    QVERIFY(authorisations->authorisationOf(zoneInfo).isEmpty());
    QTRY_VERIFY2(!check.warningText().contains(QStringLiteral("notice")), qPrintable(check.warningText()));
    QVERIFY(!check.infoText().contains(QStringLiteral("notify")));
    QCOMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 1);
    QCOMPARE(exporter.briefing().zones[0].notification, QStringLiteral("Notified, valid, reference N-1"));

    // Kept with the plan, apart from an authorisation of the zone
    QVERIFY(authorisations->authorise(zoneInfo, QStringLiteral("P-1"), QString(), QString()).isEmpty());
    QCOMPARE(authorisations->count(), 2);
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Authorised).count(), 1);
    QJsonObject planJson;
    GeoZonePlanData::save(&masterController, planJson, &manager);
    const QJsonArray saved = planJson[QString(GeoZonePlanData::jsonKey)].toObject()[QStringLiteral("authorisations")].toArray();
    QCOMPARE(saved.count(), 2);
    QCOMPARE(saved[1].toObject()[QStringLiteral("kind")].toString(), QStringLiteral("notification"));
    authorisations->revoke(QStringLiteral("CHE/NOTE"), true);
    QCOMPARE(authorisations->count(), 1);
    QVERIFY(authorisations->find(zone));
    manager.planning()->clearPlannedStart();
}

void GeoZonePlanningTest::_testBriefingDocuments()
{
    // The briefing of GeoZoneBriefingTest::_testBriefing
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2027, 1, 1), QTime(8, 0)));
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    BriefingMission mission(dir);
    QString errorString;
    QVERIFY2(mission.load(errorString), qPrintable(errorString));
    const GeoZone *const ctr = mission.zones.zoneList().constFirst();
    const GeoZoneBriefing briefing = mission.build();

    // JSON with its schema and version
    const QJsonObject json = GeoZoneBriefingExport::toJson(briefing);
    QCOMPARE(json[QStringLiteral("schema")].toString(), QStringLiteral("qgc-geozone-briefing"));
    QCOMPARE(json[QStringLiteral("version")].toInt(), 1);
    QCOMPARE(json[QStringLiteral("mission")].toObject()[QStringLiteral("name")].toString(), QStringLiteral("Survey"));
    QCOMPARE(json[QStringLiteral("mission")].toObject()[QStringLiteral("plannedStart")].toString(), QStringLiteral("2027-01-01T10:30:00Z"));
    const QJsonArray zones = json[QStringLiteral("zones")].toArray();
    QCOMPARE(zones.count(), 4);
    const QJsonObject ctrJson = zones[0].toObject();
    QCOMPARE(ctrJson[QStringLiteral("involvement")].toString(), QStringLiteral("entered"));
    QCOMPARE(ctrJson[QStringLiteral("restriction")].toString(), QStringLiteral("req_authorisation"));
    QCOMPARE(ctrJson[QStringLiteral("crossings")].toArray()[0].toObject()[QStringLiteral("entryItem")].toInt(), 1);
    QVERIFY(ctrJson[QStringLiteral("crossings")].toArray()[0].toObject()[QStringLiteral("zoneFloor")].isNull());
    QCOMPARE(ctrJson[QStringLiteral("activePeriods")].toArray()[0].toObject()[QStringLiteral("start")].toString(), QStringLiteral("2027-01-01T10:45:00Z"));
    QCOMPARE(ctrJson[QStringLiteral("authorisation")].toObject()[QStringLiteral("permit")].toString(), QStringLiteral("P-77"));
    QCOMPARE(ctrJson[QStringLiteral("contentHash")].toString(), QString::fromLatin1(ctr->data().contentHash.toHex()));
    QVERIFY(ctrJson[QStringLiteral("distance")].isNull());
    QCOMPARE(zones[2].toObject()[QStringLiteral("involvement")].toString(), QStringLiteral("contingency"));
    QCOMPARE(zones[3].toObject()[QStringLiteral("involvement")].toString(), QStringLiteral("nearby"));
    QVERIFY(zones[3].toObject()[QStringLiteral("distance")].isDouble());
    const QJsonObject sourceJson = json[QStringLiteral("sources")].toArray()[0].toObject();
    QCOMPARE(sourceJson[QStringLiteral("contentHash")].toString(), QString::fromLatin1(briefing.sources[0].contentHash.toHex()));
    QVERIFY(sourceJson.contains(QStringLiteral("airacCycle")));

    // The web page escapes the text of the zones
    const QString html = GeoZoneBriefingExport::toHtml(briefing);
    QVERIFY(html.startsWith(QStringLiteral("<!DOCTYPE html>")));
    QVERIFY(html.contains(QStringLiteral("name=\"viewport\"")));
    QVERIFY(!html.contains(QStringLiteral("<script")));
    QVERIFY(html.contains(QStringLiteral("CTR &lt;script&gt;alert(1)&lt;/script&gt;")));
    for (const QString &text : { QStringLiteral("Skyguide"), QStringLiteral("P-77"), QStringLiteral("Call the tower"), QStringLiteral("Prison"), QStringLiteral("NEAR"),
                                 QStringLiteral("RET"), QString::fromLatin1(briefing.sources[0].contentHash.toHex()) }) {
        QVERIFY2(html.contains(text), qPrintable(text));
    }
    QVERIFY(!html.contains(QStringLiteral("FAR")));
    // Counts without "(s)" plurals, English has no translation choosing the forms
    QVERIFY2(html.contains(QStringLiteral("Entered by the flight path: 2.<br>Of them restricted, active during the flight and not authorised: 1.<br>"
                                          "Entered only by returns: 1.<br>Restricted within 500 m: 1.")), qPrintable(html));
    QVERIFY(!html.contains(QStringLiteral("(s)")));

    // A4 PDF
    const QString pdfFile = dir.filePath(QStringLiteral("briefing.pdf"));
    QVERIFY2(GeoZoneBriefingExport::write(briefing, pdfFile, GeoZoneBriefingExport::Format::Pdf, errorString), qPrintable(errorString));
    QFile pdf(pdfFile);
    QVERIFY(pdf.open(QIODevice::ReadOnly));
    const QByteArray content = pdf.readAll();
    QVERIFY(content.startsWith("%PDF-"));
    QVERIFY(content.trimmed().endsWith("%%EOF"));
    QVERIFY(content.contains("/Type /Page"));
}

void GeoZonePlanningTest::_testBriefingPrint()
{
    ScopedEnvironment environment;
    environment.setNow(_utc(QDate(2027, 1, 1), QTime(8, 0)));
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    BriefingMission mission(dir);
    QString errorString;
    QVERIFY2(mission.load(errorString), qPrintable(errorString));
    const GeoZoneBriefing briefing = mission.build();

    // The application palette follows the color theme of the desktop, the document stays dark on white with a dark one.
    // Its text can be searched and copied, also in fonts with ligatures like the application's Open Sans.
    const QPalette applicationPalette = QGuiApplication::palette();
    const QFont applicationFont = QGuiApplication::font();
    const auto restore = qScopeGuard([&applicationPalette, &applicationFont]() {
        QGuiApplication::setPalette(applicationPalette);
        QGuiApplication::setFont(applicationFont);
    });
    QGuiApplication::setFont(QFont(QStringLiteral("Open Sans")));
    const auto writePdf = [&briefing, &dir](const QColor &text, const QColor &window) {
        QGuiApplication::setPalette(QPalette(text, window, window.lighter(), window.darker(), window.darker(120), text, Qt::red, window, window));
        const QString fileName = dir.filePath(QStringLiteral("briefing-%1.pdf").arg(text.name().mid(1)));
        QString errorString;
        return GeoZoneBriefingExport::writePdf(briefing, fileName, errorString) ? fileName : QString();
    };
    const QString pdfFile = writePdf(Qt::white, QColor(0x30, 0x30, 0x30));
    QVERIFY(!pdfFile.isEmpty());
    QFile pdf(pdfFile);
    QVERIFY(pdf.open(QIODevice::ReadOnly));
    int fontCount = 0;
    const QStringList unmapped = unmappedPdfGlyphs(pdf.readAll(), fontCount);
    QVERIFY(fontCount > 0);
    QVERIFY2(unmapped.isEmpty(), qPrintable(unmapped.join(QStringLiteral(", "))));

    // With the text extraction and rendering of Poppler when installed
    const QString pdftotext = QStandardPaths::findExecutable(QStringLiteral("pdftotext"));
    if (!pdftotext.isEmpty()) {
        QProcess process;
        process.start(pdftotext, { QStringLiteral("-enc"), QStringLiteral("UTF-8"), pdfFile, QStringLiteral("-") });
        QVERIFY(process.waitForFinished(30000));
        const QString text = QString::fromUtf8(process.readAllStandardOutput());
        for (const QString &word : { QStringLiteral("Geozone briefing"), QStringLiteral("official"), QStringLiteral("flight path") }) {
            QVERIFY2(text.contains(word), qPrintable(word));
        }
    }
    const QString pdftoppm = QStandardPaths::findExecutable(QStringLiteral("pdftoppm"));
    if (!pdftoppm.isEmpty()) {
        // The first page at 100 dpi
        const auto firstPage = [&pdftoppm](const QString &fileName) {
            QProcess process;
            process.start(pdftoppm, { QStringLiteral("-r"), QStringLiteral("100"), QStringLiteral("-l"), QStringLiteral("1"), QStringLiteral("-singlefile"),
                                      QStringLiteral("-png"), fileName, fileName });
            return process.waitForFinished(30000) ? QImage(fileName + QStringLiteral(".png")).convertToFormat(QImage::Format_RGB32) : QImage();
        };
        const QImage onDarkTheme = firstPage(pdfFile);
        QVERIFY(!onDarkTheme.isNull());
        QCOMPARE(onDarkTheme, firstPage(writePdf(Qt::black, Qt::white)));
        QCOMPARE(onDarkTheme.pixel(0, onDarkTheme.height() - 1), qRgb(255, 255, 255));
        // The text of the headings and tables is dark gray to black, the notes gray
        qsizetype darkPixels = 0;
        for (int y = 0; y < onDarkTheme.height(); y++) {
            const QRgb *const line = reinterpret_cast<const QRgb*>(onDarkTheme.constScanLine(y));
            for (int x = 0; x < onDarkTheme.width(); x++) {
                const int red = qRed(line[x]);
                const int green = qGreen(line[x]);
                const int blue = qBlue(line[x]);
                const bool neutral = ((std::max)({ red, green, blue }) - (std::min)({ red, green, blue })) < 16;
                if (neutral && (qGray(line[x]) < 64)) {
                    darkPixels++;
                }
            }
        }
        QVERIFY2(darkPixels > 5000, qPrintable(QString::number(darkPixels)));
    }
}

void GeoZonePlanningTest::_testBriefingExport()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        _circleFeature(QStringLiteral("A"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(1000, 90), 200),
        _circleFeature(QStringLiteral("B"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(2500, 90), 200),
    }), errorString), qPrintable(errorString));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(500, 270), 1);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(3500, 90), 2);

    // The flight path is updated after the items
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 2);

    GeoZoneBriefingExport exporter(&manager);
    exporter.setMissionController(missionController);
    QCOMPARE(exporter.defaultFileName(), QStringLiteral("geozone-briefing.pdf"));
    QVERIFY(!exporter.defaultFolder().isEmpty());

    // The zones the mission enters after its first item
    const QString jsonFile = dir.filePath(QStringLiteral("mission.json"));
    QVERIFY2(exporter.exportBriefing(jsonFile), qPrintable(exporter.errorString()));
    QFile file(jsonFile);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(json[QStringLiteral("version")].toInt(), GeoZoneBriefingExport::jsonVersion);
    const QJsonObject mission = json[QStringLiteral("mission")].toObject();
    QCOMPARE(mission[QStringLiteral("itemCount")].toInt(), 2);
    QVERIFY(mission[QStringLiteral("distanceMeters")].toDouble() > 3900);
    const QStringList returnAssumptions = mission[QStringLiteral("returnAssumptions")].toVariant().toStringList();
    QCOMPARE(returnAssumptions.value(0), QStringLiteral("Returns modeled on PX4 with its default parameters, as the vehicle's are unknown."));
    QCOMPARE(returnAssumptions.value(1), QStringLiteral("Return altitude: 30 m above the destination, or the current altitude when higher."));
    QVERIFY2(returnAssumptions.constLast().startsWith(QStringLiteral("Returns checked from positions along the mission: ")), qPrintable(returnAssumptions.constLast()));
    QVERIFY(mission[QStringLiteral("plannedStart")].isNull());
    QStringList identifiers;
    for (const QJsonValue &zone : json[QStringLiteral("zones")].toArray()) {
        if (zone.toObject()[QStringLiteral("involvement")].toString() == QStringLiteral("entered")) {
            identifiers.append(zone.toObject()[QStringLiteral("identifier")].toString());
            QCOMPARE(zone.toObject()[QStringLiteral("crossings")].toArray()[0].toObject()[QStringLiteral("entryItem")].toInt(), 1);
        }
    }
    QCOMPARE(identifiers, (QStringList{ QStringLiteral("A"), QStringLiteral("B") }));
    QCOMPARE(json[QStringLiteral("sources")].toArray().count(), 1);

    // PDF without an extension, errors reported
    QVERIFY2(exporter.exportBriefing(dir.filePath(QStringLiteral("briefing"))), qPrintable(exporter.errorString()));
    QVERIFY(QFileInfo(dir.filePath(QStringLiteral("briefing.pdf"))).size() > 0);
    QVERIFY2(exporter.exportBriefing(dir.filePath(QStringLiteral("briefing.html"))), qPrintable(exporter.errorString()));
    QVERIFY(!exporter.exportBriefing(dir.filePath(QStringLiteral("missing/briefing.json"))));
    QVERIFY(!exporter.errorString().isEmpty());
}

void GeoZonePlanningTest::_testPlanAudit()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    const QJsonObject zoneA = _circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(1000, 90), 200);
    const QJsonObject zoneB = _circleFeature(QStringLiteral("B"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(2500, 90), 200);
    const QJsonObject zoneD = _circleFeature(QStringLiteral("D"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(2000, 0), 200);
    const auto load = [&](const QJsonArray &features) {
        QString errorString;
        return manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), features), errorString);
    };
    QVERIFY(load({ zoneA, zoneB, zoneD }));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(500, 270), 1);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(3500, 90), 2);
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    const auto relevant = [&check]() {
        QStringList identifiers;
        for (const GeoZone *zone : check.relevantZones()) {
            identifiers.append(zone->identifier());
        }
        identifiers.sort();
        return identifiers;
    };
    QTRY_COMPARE(relevant(), (QStringList{ QStringLiteral("A"), QStringLiteral("B") }));

    // Saved with the hashes of the zones the mission enters and the version of the source
    QJsonObject planJson;
    GeoZonePlanData::save(&masterController, planJson, &manager);
    const QJsonObject audit = planJson[QString(GeoZonePlanData::jsonKey)].toObject()[QStringLiteral("audit")].toObject();
    QCOMPARE(audit[QStringLiteral("version")].toInt(), GeoZonePlanAudit::version);
    QCOMPARE(audit[QStringLiteral("zones")].toArray().count(), 2);
    QVERIFY(!audit[QStringLiteral("zones")].toArray()[0].toObject()[QStringLiteral("contentHash")].toString().isEmpty());
    QCOMPARE(audit[QStringLiteral("sources")].toArray().count(), 1);
    QVERIFY(manager.planning()->planAudit().has_value());
    check.update();
    QVERIFY(check.auditText().isEmpty());

    // Reopened with the same data
    manager.planning()->setPlanAudit(std::nullopt);
    GeoZonePlanData::load(nullptr, planJson, &manager);
    QVERIFY(manager.planning()->planAudit().has_value());
    check.update();
    QVERIFY(check.auditText().isEmpty());

    // Editing the mission into a zone published before isn't a change of the data
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(2000, 0), 3);
    QTRY_VERIFY(relevant().contains(QStringLiteral("D")));
    QVERIFY(check.auditText().isEmpty());
    missionController->removeVisualItem(3);
    QTRY_COMPARE(relevant(), (QStringList{ QStringLiteral("A"), QStringLiteral("B") }));

    // A zone whose geometry changed
    const QJsonObject largerA = _circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(1000, 90), 250);
    QVERIFY(load({ largerA, zoneB, zoneD }));
    check.update();
    QVERIFY2(check.auditText().contains(QStringLiteral("1 zone(s) modified")), qPrintable(check.auditText()));
    QVERIFY(check.auditText().contains(QStringLiteral("1 source(s) updated")));
    QVERIFY2(check.auditDetails().contains(QStringLiteral("Modified: A (Authorisation required)")), qPrintable(check.auditDetails().join(QChar(u'\n'))));

    // A zone removed and a new one on the path
    const QJsonObject zoneE = _circleFeature(QStringLiteral("E"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(1800, 90), 100);
    QVERIFY(load({ largerA, zoneD, zoneE }));
    check.update();
    QVERIFY(check.auditText().contains(QStringLiteral("1 new")));
    QVERIFY(check.auditText().contains(QStringLiteral("1 removed")));
    QVERIFY(check.auditDetails().contains(QStringLiteral("Removed: B (Prohibited)")));
    QVERIFY(check.auditDetails().contains(QStringLiteral("New: E (Prohibited)")));

    // Acknowledged until the plan is opened again, the file keeps the data it was saved with
    check.acknowledgeAudit();
    check.update();
    QVERIFY(check.auditText().isEmpty());
    GeoZonePlanData::load(nullptr, planJson, &manager);
    check.update();
    QVERIFY(!check.auditText().isEmpty());

    // Saving again records the current data
    QJsonObject resaved;
    GeoZonePlanData::save(&masterController, resaved, &manager);
    check.update();
    QVERIFY(check.auditText().isEmpty());
    GeoZonePlanData::load(nullptr, resaved, &manager);
    check.update();
    QVERIFY(check.auditText().isEmpty());

    // Plans without the audit load silently
    QJsonObject legacy = planJson;
    QJsonObject legacyZones = legacy[QString(GeoZonePlanData::jsonKey)].toObject();
    legacyZones.remove(QStringLiteral("audit"));
    legacy[QString(GeoZonePlanData::jsonKey)] = legacyZones;
    GeoZonePlanData::load(nullptr, legacy, &manager);
    QVERIFY(!manager.planning()->planAudit().has_value());
    check.update();
    QVERIFY(check.auditText().isEmpty());
    GeoZonePlanData::load(nullptr, planJson, &manager);
    GeoZonePlanData::load(nullptr, QJsonObject(), &manager);
    QVERIFY(!manager.planning()->planAudit().has_value());
}

void GeoZonePlanningTest::_testPlanSave()
{
    const ScopedEnvironment environment;
    TestFeedServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.files.insert("/zones", server.body);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    GeoZoneSourceRepository *const repository = manager.repository();
    repository->setFollowArea(true);

    // A service downloading its zones for the area of interest, around the vehicle for now
    repository->setVehiclePosition(QGeoCoordinate(47.0, 8.0));
    const QString url = server.url(QStringLiteral("/zones?bbox={bbox}"));
    QSignalSpy addedSpy(&manager, &GeoZoneManager::sourceAdded);
    manager.addService(url, GeoZoneService().toVariantMap(), QString());
    QVERIFY(addedSpy.wait(10000));
    QTRY_VERIFY(!manager.repository()->busy());
    QVERIFY(manager.repository()->source(url)->followsArea());

    // A mission away from the vehicle, whose zones the service would download for a check following it
    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    (void) missionController->insertSimpleMissionItem(QGeoCoordinate(47.5, 8.5), 1);
    (void) missionController->insertSimpleMissionItem(QGeoCoordinate(47.52, 8.5), 2);
    QTRY_VERIFY(!GeoZoneMissionReader::path(missionController).paths().isEmpty());

    // Saving neither sets the area of interest nor downloads, and records the audit
    const QGeoRectangle area = repository->areaOfInterest();
    const int requests = server.requestCount;
    QJsonObject planJson;
    GeoZonePlanData::save(&masterController, planJson, &manager);
    QVERIFY(!manager.repository()->busy());
    QCOMPARE(repository->areaOfInterest(), area);
    QTest::qWait(200);
    QCOMPARE(server.requestCount, requests);
    const std::optional<GeoZonePlanAudit::Record> saved =
        GeoZonePlanAudit::Record::fromJson(planJson[QString(GeoZonePlanData::jsonKey)].toObject()[QStringLiteral("audit")].toObject());
    QVERIFY(saved.has_value());
    QCOMPARE(manager.planning()->planAudit(), saved);

    // The Plan View's check follows the mission, the plan is saved with its result once settled, the same
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    QVERIFY(!check.settled());
    QVERIFY(!GeoZoneMissionCheck::settledCheck(&manager, missionController));
    QTRY_VERIFY(!manager.repository()->busy() && check.settled());
    QCOMPARE(GeoZoneMissionCheck::settledCheck(&manager, missionController), &check);
    QVERIFY(repository->areaOfInterest().contains(QGeoCoordinate(47.51, 8.5)));
    QJsonObject resaved;
    GeoZonePlanData::save(&masterController, resaved, &manager);
    QCOMPARE(resaved[QString(GeoZonePlanData::jsonKey)].toObject()[QStringLiteral("audit")].toObject(), check.auditRecord().toJson());
    // While a change waits to be checked, the plan is evaluated for saving, with the result of the check
    check.setUpdateDelay(600000);
    (void) missionController->insertSimpleMissionItem(QGeoCoordinate(47.54, 8.5), 3);
    QTRY_VERIFY(!check.settled());
    QVERIFY(!GeoZoneMissionCheck::settledCheck(&manager, missionController));
    QJsonObject edited;
    GeoZonePlanData::save(&masterController, edited, &manager);
    QVERIFY(!manager.repository()->busy());
    check.update();
    QVERIFY(check.settled());
    QCOMPARE(edited[QString(GeoZonePlanData::jsonKey)].toObject()[QStringLiteral("audit")].toObject(), check.auditRecord().toJson());
    QTRY_VERIFY(!manager.repository()->busy());
    manager.repository()->clear();
}

void GeoZonePlanningTest::_testUploadReasons()
{
    ScopedEnvironment environment;
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    const QJsonObject prohibited = _circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 200);
    const QJsonObject authorisation = _circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(600, 90), 200);
    const auto load = [&](const QJsonArray &features) {
        QString errorString;
        return manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), features), errorString);
    };
    QVERIFY(load({ prohibited, authorisation }));

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 270), 1);
    (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(1000, 90), 2);
    GeoZoneMissionCheck check(&manager);
    check.setUpdateDelay(0);
    check.setMissionController(missionController);
    check.setGeoFenceController(masterController.geoFenceController());
    QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 2);

    // The prohibited zone the fence doesn't exclude, not the zone requiring an authorisation
    QStringList reasons = check.uploadReasons();
    QCOMPARE(reasons.count(), 1);
    QVERIFY2(reasons[0].startsWith(QStringLiteral("The mission path enters prohibited geozones: P.")), qPrintable(reasons[0]));
    QVERIFY(reasons[0].contains(QStringLiteral("doesn't exclude 1 of them")));
    // Nor once authorised
    GeoZone *const zone = manager.zoneList().constFirst()->identifier() == QStringLiteral("P") ? manager.zoneList().constFirst() : manager.zoneList().constLast();
    QCOMPARE(zone->identifier(), QStringLiteral("P"));
    QVERIFY(manager.planning()->authorisations()->authorise(QVariant::fromValue(GeoZoneInfo(zone)), QStringLiteral("P-1"), QString(),
                                                            environment.now().toLocalTime().date().addDays(30).toString(Qt::ISODate)).isEmpty());
    QTRY_VERIFY(check.uploadReasons().isEmpty());
    QCOMPARE(check.zones(GeoZoneMissionCheck::Authorised).count(), 1);

    // Geozone data changed since the plan was saved, until acknowledged
    QJsonObject planJson;
    GeoZonePlanData::save(&masterController, planJson, &manager);
    QVERIFY(load({ prohibited, _circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(600, 90), 250) }));
    QTRY_VERIFY(!check.auditText().isEmpty());
    reasons = check.uploadReasons();
    QCOMPARE(reasons.count(), 1);
    QVERIFY2(reasons[0].startsWith(check.auditText()), qPrintable(reasons[0]));
    QVERIFY2(reasons[0].contains(QStringLiteral("Modified: A (Authorisation required)")), qPrintable(reasons[0]));
    check.acknowledgeAudit();
    QTRY_VERIFY(check.uploadReasons().isEmpty());

    // Outdated data treated as invalid, whose zones still apply
    environment.addSecs(8 * 24 * 3600);
    manager.repository()->updateValidity();
    QVERIFY(manager.repository()->dataInvalid());
    reasons = check.uploadReasons();
    QCOMPARE(reasons.count(), 1);
    QVERIFY2(reasons[0].startsWith(QStringLiteral("The geozone data is outdated and treated as invalid")), qPrintable(reasons[0]));
    QVERIFY2(reasons[0].contains(QStringLiteral("zones.json: Data is 8 days old")), qPrintable(reasons[0]));
    check.update();
    QCOMPARE(check.zones(GeoZoneMissionCheck::Authorised).count(), 1);
    QCOMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 1);
    manager.repository()->clear();
}

void GeoZonePlanningTest::_testPlannedStartPerPlan()
{
    // Plan files are loaded and saved with the application's manager
    GeoZonePlanning *const planning = GeoZoneManager::instance()->planning();
    const QDateTime start = _utc(QDate(2027, 1, 1), QTime(10, 0));
    const GeoZonePlanAudit::Record record = GeoZonePlanAudit::Record::make({}, {}, start);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString withStart = dir.filePath(QStringLiteral("with-start.plan"));
    const QString withoutStart = dir.filePath(QStringLiteral("without-start.plan"));
    {
        PlanMasterController masterController;
        masterController.setFlyView(false);
        masterController.start();
        (void) masterController.missionController()->insertSimpleMissionItem(QGeoCoordinate(47, 8), 1);
        planning->setPlannedStart(start);
        masterController.saveToFile(withStart);
        planning->clearPlannedStart();
        masterController.saveToFile(withoutStart);
    }

    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();

    // A plan without a planned start doesn't keep the one of the plan before
    masterController.loadFromFile(withStart);
    QCOMPARE(planning->plannedStart(), start);
    masterController.loadFromFile(withoutStart);
    QVERIFY(!planning->plannedStart().isValid());

    // A new plan has neither the planned start nor the audit record of the previous one
    masterController.loadFromFile(withStart);
    QCOMPARE(planning->plannedStart(), start);
    planning->setPlanAudit(record);
    masterController.removeAll();
    QVERIFY(!planning->plannedStart().isValid());
    QVERIFY(!planning->planAudit().has_value());
    // Nor is the start of a new plan kept when items are added and removed
    planning->setPlannedStart(start);
    (void) masterController.missionController()->insertSimpleMissionItem(QGeoCoordinate(47, 8), 1);
    masterController.missionController()->removeVisualItem(1);
    QCOMPARE(planning->plannedStart(), start);

    // The Fly View follows the mission on the vehicle, which isn't the plan
    PlanMasterController flyController;
    flyController.start();
    flyController.removeAll();
    QCOMPARE(planning->plannedStart(), start);

    // Nor does a plan downloaded from the vehicle
    _connectMockLink(MAV_AUTOPILOT_PX4);
    Vehicle *const vehicle = MultiVehicleManager::instance()->activeVehicle();
    QVERIFY(vehicle);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle->initialPlanRequestComplete(), 30000);
    QTRY_COMPARE(masterController.managerVehicle(), vehicle);
    QTRY_VERIFY(!masterController.syncInProgress());
    planning->setPlannedStart(start);
    planning->setPlanAudit(record);
    masterController.loadFromVehicle();
    QTRY_VERIFY_WITH_TIMEOUT(!planning->plannedStart().isValid(), 15000);
    QVERIFY(!planning->planAudit().has_value());
    QTRY_VERIFY(!masterController.syncInProgress());
    _disconnectMockLink();
}

void GeoZonePlanningTest::_testProfileBands()
{
    const QGeoCoordinate center(47, 8);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const auto withLimits = [](QJsonObject feature, double lower, const QString &lowerReference, double upper, const QString &upperReference, const QString &units) {
        QJsonArray geometry = feature[QStringLiteral("geometry")].toArray();
        QJsonObject layer = geometry[0].toObject();
        layer[QStringLiteral("lowerLimit")] = lower;
        layer[QStringLiteral("lowerVerticalReference")] = lowerReference;
        layer[QStringLiteral("upperLimit")] = upper;
        layer[QStringLiteral("upperVerticalReference")] = upperReference;
        layer[QStringLiteral("uomDimensions")] = units;
        geometry[0] = layer;
        feature[QStringLiteral("geometry")] = geometry;
        return feature;
    };
    // From the ground to 120 m above it in the middle, 1000 to 2000 ft AMSL over the east end, a zone without
    // restriction and one away from the path
    const QString fileName = _writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        withLimits(_circleFeature(QStringLiteral("P"), QStringLiteral("PROHIBITED"), center, 300), 0, QStringLiteral("AGL"), 120, QStringLiteral("AGL"), QStringLiteral("M")),
        // ED-269 circle radii are in the layer units too, 200 m
        withLimits(_circleFeature(QStringLiteral("A"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(1000, 90), 200 / 0.3048), 1000, QStringLiteral("AMSL"), 2000, QStringLiteral("AMSL"), QStringLiteral("FT")),
        _circleFeature(QStringLiteral("N"), QStringLiteral("NO_RESTRICTION"), center, 500),
        _circleFeature(QStringLiteral("F"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(5000, 0), 300),
        _circleFeature(QStringLiteral("END"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(179000, 90), 300),
    });
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(fileName, errorString), qPrintable(errorString));

    // West to east over flat ground at 400 m, then a pattern without a flight path, then east again without terrain
    GeoZoneMissionPath::Segment west;
    west.from = center.atDistanceAndAzimuth(1000, 270);
    west.to = center;
    west.distance = 1000;
    west.terrainSpacing = 100;
    west.terrainAMSL = QList<double>(11, 400);
    GeoZoneMissionPath::Segment east = west;
    east.from = center;
    east.to = center.atDistanceAndAzimuth(1000, 90);
    GeoZoneMissionPath::Segment gap;
    gap.distance = 250;
    GeoZoneMissionPath::Segment beyond;
    beyond.from = center.atDistanceAndAzimuth(1000, 90);
    beyond.to = center.atDistanceAndAzimuth(1300, 90);
    beyond.distance = 300;

    const QVariantList bands = GeoZoneProfile::bandsForSegments({ west, east, gap, beyond }, manager.catalog());
    // Bands with unbounded limits compare equal, so recomputing unchanged bands doesn't repaint
    QCOMPARE(GeoZoneProfile::computeBands({ west, east, gap, beyond }, manager.catalog()), GeoZoneProfile::computeBands({ west, east, gap, beyond }, manager.catalog()));
    QStringList names;
    for (const QVariant &band : bands) {
        names.append(band.toMap()[QStringLiteral("name")].toString());
    }
    // Most restrictive last, then along the path. The gap splits the stretch through A.
    QCOMPARE(names, QStringList({ QStringLiteral("A"), QStringLiteral("A"), QStringLiteral("P") }));
    // Entered at unknown altitudes, not when flown above them
    for (const QVariant &band : bands) {
        QVERIFY(band.toMap()[QStringLiteral("entered")].toBool());
    }
    GeoZoneMissionPath::Segment westAbove = west;
    westAbove.fromAltitudeAMSL = 700;
    westAbove.toAltitudeAMSL = 700;
    GeoZoneMissionPath::Segment eastAbove = east;
    eastAbove.fromAltitudeAMSL = 700;
    eastAbove.toAltitudeAMSL = 450;
    const QList<GeoZoneProfile::Band> above = GeoZoneProfile::computeBands({ westAbove, eastAbove }, manager.catalog());
    QCOMPARE(above.count(), 2);
    for (const GeoZoneProfile::Band &band : above) {
        // Descending into A at the end of the east segment, above P in the middle
        QCOMPARE(band.entered, band.name == QStringLiteral("A"));
    }

    const auto points = [&bands](int index) { return bands[index].toMap()[QStringLiteral("points")].toList(); };
    const auto value = [](const QVariant &point, const char *key) { return point.toMap()[QString::fromLatin1(key)].toDouble(); };

    // Continuous across the segments through the middle, from the ground to 120 m above it
    const QVariantList prohibited = points(2);
    QVERIFY(qAbs(value(prohibited.first(), "distance") - 700) <= GeoZoneProfile::minSampleSpacing);
    QVERIFY(qAbs(value(prohibited.last(), "distance") - 1300) <= GeoZoneProfile::minSampleSpacing);
    QVERIFY(qIsNaN(value(prohibited.first(), "floor")));
    QCOMPARE(value(prohibited.first(), "ceiling"), 520.0);

    const QVariantList authorisation = points(0);
    QVERIFY(qAbs(value(authorisation.first(), "distance") - 1800) <= GeoZoneProfile::minSampleSpacing);
    QCOMPARE(value(authorisation.last(), "distance"), 2000.0);
    QCOMPARE(value(authorisation.first(), "floor"), 1000 * 0.3048);
    QCOMPARE(value(authorisation.first(), "ceiling"), 2000 * 0.3048);
    // After the gap, beyond the end of the segment through its center
    const QVariantList afterGap = points(1);
    QCOMPARE(value(afterGap.first(), "distance"), 2250.0);
    QVERIFY(qAbs(value(afterGap.last(), "distance") - 2450) <= GeoZoneProfile::minSampleSpacing);

    // Limits above unknown terrain are unknown
    const QVariantList unknownTerrain = GeoZoneProfile::bandsForSegments({ beyond }, manager.catalog());
    QCOMPARE(unknownTerrain.count(), 1);
    beyond.from = center.atDistanceAndAzimuth(100, 270);
    beyond.to = center.atDistanceAndAzimuth(100, 90);
    const QVariantList noTerrain = GeoZoneProfile::bandsForSegments({ beyond }, manager.catalog());
    QCOMPARE(noTerrain.count(), 1);
    QVERIFY(qIsNaN(value(noTerrain[0].toMap()[QStringLiteral("points")].toList().first(), "ceiling")));

    // Long paths are sampled more sparsely up to their end
    QList<GeoZoneMissionPath::Segment> longPath;
    for (int i = 0; i < 60; i++) {
        GeoZoneMissionPath::Segment leg;
        leg.from = center.atDistanceAndAzimuth(i * 3000, 90);
        leg.to = center.atDistanceAndAzimuth((i + 1) * 3000, 90);
        leg.distance = 3000;
        longPath.append(leg);
    }
    const QVariantList longBands = GeoZoneProfile::bandsForSegments(longPath, manager.catalog());
    const auto end = std::find_if(longBands.cbegin(), longBands.cend(), [](const QVariant &band) {
        return band.toMap()[QStringLiteral("name")].toString() == QStringLiteral("END");
    });
    QVERIFY(end != longBands.cend());
    const QVariantList endPoints = end->toMap()[QStringLiteral("points")].toList();
    const double longSpacing = 180000.0 / GeoZoneProfile::maxSamples;
    QVERIFY(qAbs(value(endPoints.first(), "distance") - 178700) <= longSpacing + 1);
    QVERIFY(qAbs(value(endPoints.last(), "distance") - 179300) <= longSpacing + 1);
}

void GeoZonePlanningTest::_testOperatorZones()
{
    ScopedEnvironment environment;
    const QGeoCoordinate center(47, 8);
    const QGeoRectangle view(center.atDistanceAndAzimuth(4000, 315), center.atDistanceAndAzimuth(4000, 135));
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString storage = dir.filePath(QStringLiteral("storage"));
    {
        GeoZoneManager manager(storage);
        GeoZoneOperatorZones *const zones = manager.operatorZones();
        QVERIFY(zones->zones().isEmpty());

        // A prohibited square drawn in the middle of the view, from the ground to 120 m
        zones->startPolygon(view);
        QVERIFY(zones->editing());
        QCOMPARE(zones->editIndex(), -1);
        QCOMPARE(zones->polygon()->count(), 4);
        QVERIFY(zones->polygon()->containsCoordinate(center));
        QVERIFY(!zones->polygon()->containsCoordinate(center.atDistanceAndAzimuth(1500, 90)));
        QVariantMap properties = zones->editProperties();
        QCOMPARE(properties.value(QStringLiteral("restriction")).toInt(), static_cast<int>(GeoZone::Prohibited));
        properties.insert(QStringLiteral("name"), QStringLiteral("Quarry"));
        properties.insert(QStringLiteral("lower"), 0);
        properties.insert(QStringLiteral("upper"), 120);
        QVERIFY(zones->saveEdit(properties));
        QVERIFY(!zones->editing());

        // Kept as an ED-318 source like any other
        GeoZoneSource *const source = manager.repository()->source(zones->fileName());
        QVERIFY(source);
        QCOMPARE(source->name(), QStringLiteral("My zones"));
        QCOMPARE(source->format(), GeoZoneParser::Format::ED318);
        QCOMPARE(manager.zoneCount(), 1);
        const GeoZone *const quarry = manager.catalog().zones().constFirst();
        QCOMPARE(quarry->name(), QStringLiteral("Quarry"));
        QCOMPARE(quarry->restriction(), GeoZone::Prohibited);
        QVERIFY(quarry->identifier().startsWith(QStringLiteral("MY-")));
        QVERIFY(!quarry->contains(GeoZone::Position{ center, 700, 500 }));
        QVERIFY(quarry->contains(GeoZone::Position{ center, 600, 500 }));

        // The mission check reports a mission through it
        PlanMasterController masterController;
        masterController.setFlyView(false);
        masterController.start();
        MissionController *const missionController = masterController.missionController();
        GeoZoneMissionCheck check(&manager);
        check.setUpdateDelay(0);
        check.setMissionController(missionController);
        (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(2000, 270), 1);
        (void) missionController->insertSimpleMissionItem(center.atDistanceAndAzimuth(2000, 90), 2);
        QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Crossed).count(), 1);
        QCOMPARE(check.zones(GeoZoneMissionCheck::Crossed)[0]->name(), QStringLiteral("Quarry"));

        // Editing it updates the source, keeping its identifier
        zones->startEdit(0);
        QCOMPARE(zones->editIndex(), 0);
        QCOMPARE(zones->polygon()->count(), 4);
        properties = zones->editProperties();
        QCOMPARE(properties.value(QStringLiteral("name")).toString(), QStringLiteral("Quarry"));
        QCOMPARE(properties.value(QStringLiteral("upper")).toDouble(), 120.0);
        QCOMPARE(properties.value(QStringLiteral("upperReference")).toString(), QStringLiteral("AGL"));
        properties.insert(QStringLiteral("restriction"), static_cast<int>(GeoZone::NoRestriction));
        const QString identifier = quarry->identifier();
        QVERIFY(zones->saveEdit(properties));
        QTRY_COMPARE(check.zones(GeoZoneMissionCheck::Permitted).count(), 1);
        QVERIFY(check.zones(GeoZoneMissionCheck::Crossed).isEmpty());
        QCOMPARE(manager.catalog().zones().constFirst()->identifier(), identifier);

        // A circle, which a shape without area can't replace
        zones->startCircle(QGeoRectangle(center.atDistanceAndAzimuth(20000, 0), 0.02, 0.02));
        QVERIFY(zones->editCircle());
        QVERIFY(zones->saveEdit(zones->editProperties()));
        QCOMPARE(zones->zones().count(), 2);
        QCOMPARE(manager.zoneCount(), 2);
        zones->startEdit(1);
        zones->circle()->radius()->setRawValue(0);
        QSignalSpy failedSpy(zones, &GeoZoneOperatorZones::failed);
        QVERIFY(!zones->saveEdit(zones->editProperties()));
        QCOMPARE(failedSpy.count(), 1);
        QVERIFY(zones->editing());
        zones->cancelEdit();
        QCOMPARE(manager.zoneCount(), 2);
    }

    // The zones are kept with the source for the next start, and removing the last removes the source
    GeoZoneManager restarted(storage);
    restarted.repository()->loadStoredSources(GeoZoneSourceRepository::LoadMode::Synchronous);
    GeoZoneOperatorZones *const zones = restarted.operatorZones();
    QCOMPARE(zones->zones().count(), 2);
    QCOMPARE(restarted.zoneCount(), 2);
    QVERIFY(zones->removeZone(0));
    QCOMPARE(restarted.zoneCount(), 1);
    QCOMPARE(zones->zones().first().toMap().value(QStringLiteral("circle")).toBool(), true);
    QVERIFY(zones->removeZone(0));
    QVERIFY(!restarted.repository()->hasSource(zones->fileName()));
    QVERIFY(!QFile::exists(zones->fileName()));
    QCOMPARE(restarted.zoneCount(), 0);

    // Removing the source in the settings removes the zones
    zones->startCircle(view);
    QVERIFY(zones->saveEdit(zones->editProperties()));
    QVERIFY(restarted.repository()->hasSource(zones->fileName()));
    restarted.repository()->clear();
    QVERIFY(zones->zones().isEmpty());
    QVERIFY(!QFile::exists(zones->fileName()));
}

void GeoZonePlanningTest::_testOperatorZonesFile()
{
    const ScopedEnvironment environment;
    const QGeoCoordinate center(47, 8);
    const QGeoRectangle view(center.atDistanceAndAzimuth(4000, 315), center.atDistanceAndAzimuth(4000, 135));
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto contents = [](const QString &fileName) {
        QFile file(fileName);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const auto write = [](const QString &fileName, const QByteArray &data) {
        QFile file(fileName);
        return QDir().mkpath(QFileInfo(fileName).absolutePath()) && file.open(QIODevice::WriteOnly) && (file.write(data) == data.size());
    };

    // The zones of a previous session, listed from their file before the stored sources are loaded
    const QString storage = dir.filePath(QStringLiteral("storage"));
    QString fileName;
    {
        GeoZoneManager manager(storage);
        manager.operatorZones()->startCircle(view);
        QVERIFY(manager.operatorZones()->saveEdit(manager.operatorZones()->editProperties()));
        fileName = manager.operatorZones()->fileName();
    }
    const QByteArray saved = contents(fileName);
    QVERIFY(!saved.isEmpty());
    {
        GeoZoneManager manager(storage);
        GeoZoneOperatorZones *const zones = manager.operatorZones();
        QCOMPARE(zones->zones().count(), 1);
        QVERIFY(zones->loadError().isEmpty());
        QVERIFY(!manager.repository()->hasSource(fileName));
        // Removing another source meanwhile leaves the zones and their file
        QString errorString;
        const QString other = _writeFeatures(dir, QStringLiteral("other.json"), QJsonArray{ _circleFeature(QStringLiteral("O"), QStringLiteral("PROHIBITED"), center, 300) });
        QVERIFY2(manager.repository()->loadFile(other, errorString), qPrintable(errorString));
        QCOMPARE(manager.repository()->sources().count(), 1);
        manager.repository()->removeSource(0);
        QVERIFY(manager.repository()->sources().isEmpty());
        QCOMPARE(zones->zones().count(), 1);
        QCOMPARE(contents(fileName), saved);
    }

    // A file which can't be read is told, and kept beside the zones saved next
    const QString unreadableStorage = dir.filePath(QStringLiteral("unreadable"));
    const QString unreadable = QDir(unreadableStorage).filePath(QStringLiteral("operator/my-zones.json"));
    const QByteArray damaged = saved.first(saved.size() / 2);
    QVERIFY(write(unreadable, damaged));
    {
        GeoZoneManager manager(unreadableStorage);
        GeoZoneOperatorZones *const zones = manager.operatorZones();
        QCOMPARE(QFileInfo(zones->fileName()), QFileInfo(unreadable));
        QVERIFY(zones->zones().isEmpty());
        QVERIFY2(zones->loadError().contains(QStringLiteral("can't be read")) && zones->loadError().contains(zones->fileName()), qPrintable(zones->loadError()));
        // Nothing to remove, the file stays
        QString errorString;
        QVERIFY(zones->setZones({}, errorString));
        QCOMPARE(contents(unreadable), damaged);

        QSignalSpy loadErrorSpy(zones, &GeoZoneOperatorZones::loadErrorChanged);
        zones->startCircle(view);
        QVERIFY(zones->saveEdit(zones->editProperties()));
        QCOMPARE(zones->zones().count(), 1);
        QCOMPARE(manager.zoneCount(), 1);
        QVERIFY(zones->loadError().isEmpty());
        QCOMPARE(loadErrorSpy.count(), 1);
        const QDir operatorDir = QFileInfo(unreadable).dir();
        const QStringList kept = operatorDir.entryList({ QStringLiteral("my-zones.unreadable-*.json") }, QDir::Files);
        QCOMPARE(kept.count(), 1);
        QCOMPARE(contents(operatorDir.filePath(kept.constFirst())), damaged);
        QVERIFY(contents(unreadable) != damaged);
    }

    // An empty file has no zones, and isn't a file which can't be read
    const QString emptyStorage = dir.filePath(QStringLiteral("empty"));
    QVERIFY(write(QDir(emptyStorage).filePath(QStringLiteral("operator/my-zones.json")), QByteArray()));
    GeoZoneManager manager(emptyStorage);
    QVERIFY(manager.operatorZones()->zones().isEmpty());
    QVERIFY2(manager.operatorZones()->loadError().isEmpty(), qPrintable(manager.operatorZones()->loadError()));
}

void GeoZonePlanningTest::_testOperatorZonesRemovalFailure()
{
#ifndef Q_OS_UNIX
    QSKIP("Needs the file permissions of Unix");
#else
    if (::geteuid() == 0) {
        QSKIP("The administrator can delete files in read-only directories");
    }
    const ScopedEnvironment environment;
    const QGeoCoordinate center(47, 8);
    const QGeoRectangle view(center.atDistanceAndAzimuth(4000, 315), center.atDistanceAndAzimuth(4000, 135));
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString storage = dir.filePath(QStringLiteral("storage"));
    GeoZoneManager manager(storage);
    GeoZoneOperatorZones *const zones = manager.operatorZones();
    zones->startCircle(view);
    QVERIFY(zones->saveEdit(zones->editProperties()));
    const QString fileName = zones->fileName();
    const QString operatorDir = QFileInfo(fileName).absolutePath();
    const QFileDevice::Permissions readOnly = QFileDevice::ReadOwner | QFileDevice::ExeOwner;
    const QFileDevice::Permissions writable = readOnly | QFileDevice::WriteOwner;
    // The temporary directory is deleted with its contents
    const auto restore = qScopeGuard([&operatorDir, &fileName, writable]() {
        (void) QFile::setPermissions(operatorDir, writable);
        (void) QFile::setPermissions(fileName, writable);
    });

    // Neither deleted nor emptied: the zone stays, listed and loaded, and the pilot is told
    QVERIFY(QFile::setPermissions(operatorDir, readOnly));
    QVERIFY(QFile::setPermissions(fileName, QFileDevice::ReadOwner));
    QSignalSpy failedSpy(zones, &GeoZoneOperatorZones::failed);
    QVERIFY(!zones->removeZone(0));
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY2(failedSpy[0][0].toString().contains(QStringLiteral("can't be removed")), qPrintable(failedSpy[0][0].toString()));
    QCOMPARE(zones->zones().count(), 1);
    QCOMPARE(manager.zoneCount(), 1);
    QVERIFY(QFileInfo(fileName).size() > 0);

    // Nor when the source is removed in the settings: it is loaded again, with the zones left in the file
    QVERIFY(manager.repository()->hasSource(fileName));
    manager.repository()->clear();
    QCOMPARE(failedSpy.count(), 2);
    QCOMPARE(zones->zones().count(), 1);
    QTRY_VERIFY(manager.repository()->hasSource(fileName));
    QCOMPARE(manager.zoneCount(), 1);

    // A file which can't be deleted is emptied, so that its zone doesn't come back at the next start
    QVERIFY(QFile::setPermissions(fileName, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    QVERIFY(zones->removeZone(0));
    QCOMPARE(failedSpy.count(), 2);
    QVERIFY(zones->zones().isEmpty());
    QVERIFY(!manager.repository()->hasSource(fileName));
    QVERIFY(QFile::exists(fileName));
    QCOMPARE(QFileInfo(fileName).size(), 0);
    GeoZoneManager restarted(storage);
    QVERIFY(restarted.operatorZones()->zones().isEmpty());
    QVERIFY(restarted.operatorZones()->loadError().isEmpty());
#endif
}

void GeoZonePlanningTest::_testMapModel()
{
    const QGeoRectangle view(QGeoCoordinate(47.1, 7.85), QGeoCoordinate(46.9, 8.15));
    const QJsonArray features = {
        _polygonFeature(QStringLiteral("BIG"), QStringLiteral("PROHIBITED"), _circleRing(QGeoCoordinate(47.0, 8.0), 4000, 2000)),
        _circleFeature(QStringLiteral("TINY"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47.01, 8.01), 20),
        _circleFeature(QStringLiteral("NEAR"), QStringLiteral("CONDITIONAL"), QGeoCoordinate(47.0, 8.2), 500),     // Within the view margin
        _circleFeature(QStringLiteral("FAR"), QStringLiteral("PROHIBITED"), QGeoCoordinate(50.0, 15.0), 1000),
    };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), features), errorString), qPrintable(errorString));

    GeoZoneMapModel model(&manager);
    QSignalSpy shapesSpy(&model, &GeoZoneMapModel::shapesChanged);
    const auto identifiers = [&model]() {
        QStringList result;
        for (const GeoZoneShape *shape : model.shapes()) {
            result.append(shape->zone()->identifier());
        }
        result.sort();
        return result;
    };
    const auto polygon = [&model]() {
        const QSet<const GeoZoneShape*> &shapes = model.shapes();
        const auto it = std::find_if(shapes.cbegin(), shapes.cend(), [](const GeoZoneShape *shape) { return !shape->isCircle(); });
        return (it != shapes.cend()) ? *it : nullptr;
    };

    // Only shapes in or near the view
    model.setView(view, 10, false);
    QCOMPARE(identifiers(), QStringList({ QStringLiteral("BIG"), QStringLiteral("NEAR"), QStringLiteral("TINY") }));
    QCOMPARE(model.detailLevel(), 10);
    QCOMPARE(model.hiddenZoneCount(), 0);
    QCOMPARE(model.labelZones().count(), 0);
    const GeoZoneShape *const polygonShape = polygon();
    QVERIFY(polygonShape);

    // Zooming in keeps the shapes and adds detail. Only zones centered in the view are labelled.
    shapesSpy.clear();
    model.setView(view, 16.5, true);
    QCOMPARE(shapesSpy.count(), 1);
    QCOMPARE(model.detailLevel(), 16);
    QCOMPARE(polygon(), polygonShape);
    QCOMPARE(model.labelZones().count(), 2);
    model.setView(view, 16, true);
    QCOMPARE(shapesSpy.count(), 1);

    // Another file adds shapes, removing a source removes its shapes
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("other.json"),
        { _circleFeature(QStringLiteral("OTHER"), QStringLiteral("PROHIBITED"), QGeoCoordinate(47.0, 8.0), 1000) }), errorString), qPrintable(errorString));
    QVERIFY(identifiers().contains(QStringLiteral("OTHER")));
    // Shapes of the other sources are kept when a source reloads
    QCOMPARE(polygon(), polygonShape);
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("other.json"),
        { _circleFeature(QStringLiteral("OTHER"), QStringLiteral("CONDITIONAL"), QGeoCoordinate(47.0, 8.0), 1000) }), errorString), qPrintable(errorString));
    QCOMPARE(polygon(), polygonShape);
    QVERIFY(identifiers().contains(QStringLiteral("OTHER")));
    // Shapes are left out before their source deletes them
    shapesSpy.clear();
    qsizetype changesBeforeRemoval = -1;
    const QMetaObject::Connection removal = connect(&manager, &GeoZoneManager::zonesAboutToChange, &model, [&]() { changesBeforeRemoval = shapesSpy.count(); });
    manager.repository()->removeSource(0);
    (void) disconnect(removal);
    QCOMPARE(changesBeforeRemoval, 1);
    QCOMPARE(identifiers(), QStringList({ QStringLiteral("OTHER") }));

    // Restrictions hidden on maps leave out their shapes and labels
    const auto restoreRestrictions = qScopeGuard([&manager]() { manager.display()->setRestrictionVisible(GeoZone::Conditional, true); });
    QVERIFY(!model.labelZones().isEmpty());
    manager.display()->setRestrictionVisible(GeoZone::Conditional, false);
    QVERIFY(!manager.display()->restrictionVisible(GeoZone::Conditional));
    QVERIFY(model.shapes().isEmpty());
    QVERIFY(model.labelZones().isEmpty());
    manager.display()->setRestrictionVisible(GeoZone::Conditional, true);
    QCOMPARE(identifiers(), QStringList({ QStringLiteral("OTHER") }));

    model.setView(QGeoRectangle(), 16, true);
    QVERIFY(model.shapes().isEmpty());
    QCOMPARE(model.labelZones().count(), 0);

    model.setView(view, 16, true);
    manager.repository()->clear();
    QVERIFY(model.shapes().isEmpty());
}

void GeoZonePlanningTest::_testMapModelShapeLimit()
{
    // More zones in view than can be shown, with growing radius
    QJsonArray features;
    const int extraZones = 10;
    for (int i = 0; i < GeoZoneMapModel::maxShapeCount + extraZones; i++) {
        const QGeoCoordinate center(47.0 + (i / 50) * 0.005, 8.0 + (i % 50) * 0.005);
        features.append(_circleFeature(QStringLiteral("Z%1").arg(i), QStringLiteral("PROHIBITED"), center, 100 + i));
    }
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), features), errorString), qPrintable(errorString));

    // The smallest zones are dropped and not labelled
    GeoZoneMapModel model(&manager);
    model.setView(manager.boundingBox(), 12, true);
    QCOMPARE(model.shapes().count(), GeoZoneMapModel::maxShapeCount);
    QCOMPARE(model.hiddenZoneCount(), extraZones);
    for (const GeoZoneShape *shape : model.shapes()) {
        QVERIFY(shape->radius() >= 100 + extraZones);
    }
    QVERIFY(model.labelZones().count() > 0);
    for (int i = 0; i < model.labelZones().count(); i++) {
        QVERIFY(GeoZoneInfo::fromVariant(model.labelZones().at(i))->identifier().mid(1).toInt() >= extraZones);
    }
}

void GeoZonePlanningTest::_testMapModelLabels()
{
    // Labels which would overlap are left out, those of the most restrictive and then the largest zones placed first
    const QGeoCoordinate center(47, 8);
    const QGeoCoordinate east = center.atDistanceAndAzimuth(3000, 90);
    const QGeoCoordinate west = center.atDistanceAndAzimuth(3000, 270);
    const QJsonArray features = {
        _circleFeature(QStringLiteral("COND"), QStringLiteral("CONDITIONAL"), center.atDistanceAndAzimuth(30, 0), 1500),
        _circleFeature(QStringLiteral("PROH"), QStringLiteral("PROHIBITED"), center, 200),
        _circleFeature(QStringLiteral("SMALL"), QStringLiteral("REQ_AUTHORISATION"), east.atDistanceAndAzimuth(20, 180), 100),
        _circleFeature(QStringLiteral("LARGE"), QStringLiteral("REQ_AUTHORISATION"), east, 400),
        _circleFeature(QStringLiteral("FREE"), QStringLiteral("NO_RESTRICTION"), west, 300),
    };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), features), errorString), qPrintable(errorString));

    GeoZoneMapModel model(&manager);
    const auto labels = [&model]() {
        QStringList identifiers;
        for (const QVariant &value : model.labelZones()) {
            identifiers.append(GeoZoneInfo::fromVariant(value)->identifier());
        }
        identifiers.sort();
        return identifiers;
    };
    const QGeoRectangle view(center.atDistanceAndAzimuth(6000, 315), center.atDistanceAndAzimuth(6000, 135));
    // About 13 m per pixel: the centers 30 and 20 m apart overlap, zones without restriction aren't labelled
    model.setView(view, 13, true);
    QCOMPARE(labels(), QStringList({ QStringLiteral("LARGE"), QStringLiteral("PROH") }));
    // About 0.8 m per pixel
    model.setView(view, 17, true);
    QCOMPARE(labels(), QStringList({ QStringLiteral("COND"), QStringLiteral("LARGE"), QStringLiteral("PROH"), QStringLiteral("SMALL") }));
    // Wider labels overlap again
    model.setLabelCharWidth(50);
    QVERIFY(labels().count() < 4);
    model.setLabelCharWidth(7);

    // Highlighted zones without restriction are labelled
    const QList<const GeoZone*> freeZones = manager.catalog().zonesAt(west);
    QCOMPARE(freeZones.count(), 1);
    model.setHighlightedZones(GeoZoneInfo::toVariantList(freeZones));
    QVERIFY(labels().contains(QStringLiteral("FREE")));
    model.setHighlightedZones({});
    QVERIFY(!labels().contains(QStringLiteral("FREE")));

    // Labels keep clear of the mission items
    model.setView(view, 13, true);
    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    model.setMissionController(missionController);
    (void) missionController->insertSimpleMissionItem(east, 1);
    QTRY_COMPARE(labels(), QStringList({ QStringLiteral("PROH") }));
}

void GeoZonePlanningTest::_testMapModelFilters()
{
    ScopedEnvironment environment;
    const QDateTime now = _utc(QDate(2026, 6, 15), QTime(10, 0));
    environment.setNow(now);
    const QGeoCoordinate center(47, 8);
    const auto withLimits = [](QJsonObject feature, double lower, const QString &lowerReference, double upper, const QString &upperReference) {
        QJsonArray geometry = feature[QStringLiteral("geometry")].toArray();
        QJsonObject layer = geometry[0].toObject();
        layer[QStringLiteral("uomDimensions")] = QStringLiteral("M");
        layer[QStringLiteral("lowerLimit")] = lower;
        layer[QStringLiteral("lowerVerticalReference")] = lowerReference;
        layer[QStringLiteral("upperLimit")] = upper;
        layer[QStringLiteral("upperVerticalReference")] = upperReference;
        geometry[0] = layer;
        feature[QStringLiteral("geometry")] = geometry;
        return feature;
    };
    QJsonObject later = _circleFeature(QStringLiteral("LATER"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(2000, 270), 300);
    later[QStringLiteral("applicability")] = QJsonArray{ QJsonObject{ { "permanent", "NO" },
        { "startDateTime", now.addSecs(6 * 3600).toString(Qt::ISODate) }, { "endDateTime", now.addSecs(8 * 3600).toString(Qt::ISODate) } } };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    GeoZoneManager manager(dir.filePath(QStringLiteral("storage")));
    QString errorString;
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("zones.json"), QJsonArray{
        withLimits(_circleFeature(QStringLiteral("LOW"), QStringLiteral("PROHIBITED"), center, 300), 0, QStringLiteral("AGL"), 60, QStringLiteral("AGL")),
        withLimits(_circleFeature(QStringLiteral("HIGH"), QStringLiteral("PROHIBITED"), center.atDistanceAndAzimuth(2000, 90), 300),
                   1500, QStringLiteral("AMSL"), 3000, QStringLiteral("AMSL")),
        _circleFeature(QStringLiteral("ALL"), QStringLiteral("CONDITIONAL"), center.atDistanceAndAzimuth(2000, 0), 300),
        later,
    }), errorString), qPrintable(errorString));
    QVERIFY2(manager.repository()->loadFile(_writeFeatures(dir, QStringLiteral("other.json"), QJsonArray{
        _circleFeature(QStringLiteral("OTHER"), QStringLiteral("REQ_AUTHORISATION"), center.atDistanceAndAzimuth(2000, 180), 300),
    }), errorString), qPrintable(errorString));

    GeoZoneSettings *const settings = GeoZoneSettings::instance();
    GeoZoneDisplay *const display = manager.display();
    const auto restoreFilters = qScopeGuard([display]() { display->resetFilters(); });

    GeoZoneMapModel model(&manager);
    const QGeoRectangle view(center.atDistanceAndAzimuth(5000, 315), center.atDistanceAndAzimuth(5000, 135));
    model.setView(view, 12, false);
    const auto shown = [&model]() {
        QStringList identifiers;
        for (const GeoZoneShape *shape : model.shapes()) {
            identifiers.append(shape->zone()->identifier());
        }
        identifiers.sort();
        return identifiers.join(QLatin1Char(' '));
    };
    const auto zone = [&manager](const QString &identifier) {
        const QList<GeoZone*> &zones = manager.zoneList();
        return *std::find_if(zones.cbegin(), zones.cend(), [&identifier](const GeoZone *z) { return z->identifier() == identifier; });
    };
    const auto altitudes = [](double altitudeAMSL, double terrainAMSL) {
        GeoZoneAltitudeRange range;
        range.add(GeoZone::Position{ QGeoCoordinate(47, 8), altitudeAMSL, terrainAMSL });
        return range;
    };
    QCOMPARE(shown(), QStringLiteral("ALL HIGH LATER LOW OTHER"));
    QCOMPARE(model.filteredZoneCount(), 0);
    QVERIFY(!display->filtered());

    // Restrictions hidden in the legend, except kept and highlighted zones, also at map clicks
    display->setRestrictionVisible(GeoZone::Prohibited, false);
    QVERIFY(display->filtered());
    QCOMPARE(shown(), QStringLiteral("ALL OTHER"));
    QCOMPARE(model.filteredZoneCount(), 3);
    QCOMPARE(model.zonesAt(center).count(), 0);
    model.setKeptZones(GeoZoneInfo::toVariantList(QList<const GeoZone*>{ zone(QStringLiteral("LOW")) }));
    model.setHighlightedZones(GeoZoneInfo::toVariantList(QList<const GeoZone*>{ zone(QStringLiteral("HIGH")) }));
    QCOMPARE(shown(), QStringLiteral("ALL HIGH LOW OTHER"));
    QCOMPARE(model.filteredZoneCount(), 1);
    QCOMPARE(model.zonesAt(center).count(), 1);
    model.setKeptZones({});
    model.setHighlightedZones({});
    display->resetFilters();
    QVERIFY(!display->filtered());
    QCOMPARE(shown(), QStringLiteral("ALL HIGH LATER LOW OTHER"));

    // Altitudes: all zones without them, then those within the margin of the vehicle above the terrain or sea level
    settings->altitudeFilter()->setRawValue(true);
    QCOMPARE(shown(), QStringLiteral("ALL HIGH LATER LOW OTHER"));
    model.setAltitudeRange(altitudes(500, 450));
    QCOMPARE(shown(), QStringLiteral("ALL LATER LOW OTHER"));
    QCOMPARE(model.filteredZoneCount(), 1);
    model.setAltitudeRange(altitudes(1450, 450));
    QCOMPARE(shown(), QStringLiteral("ALL HIGH LATER OTHER"));
    model.setAltitudeRange(altitudes(1500 - GeoZoneMapFilter::altitudeMargin - 2, 450));
    QCOMPARE(shown(), QStringLiteral("ALL LATER OTHER"));
    // Limits above ground are kept without the terrain
    model.setAltitudeRange(altitudes(1450, qQNaN()));
    QCOMPARE(shown(), QStringLiteral("ALL HIGH LATER LOW OTHER"));
    // A range of altitudes, like those of a mission climbing from the ground
    GeoZoneAltitudeRange mission = altitudes(450, 450);
    mission.add(GeoZone::Position{ center, 1450, 450 });
    model.setAltitudeRange(mission);
    QCOMPARE(shown(), QStringLiteral("ALL HIGH LATER LOW OTHER"));
    model.setAltitudeRange(altitudes(500, 450));

    // Schedules: active now, or within the hours ahead
    settings->scheduleFilter()->setRawValue(GeoZoneMapFilter::ActiveNow);
    QCOMPARE(shown(), QStringLiteral("ALL LOW OTHER"));
    QCOMPARE(model.filteredZoneCount(), 2);
    settings->scheduleFilter()->setRawValue(GeoZoneMapFilter::ActiveSoon);
    QCOMPARE(shown(), QStringLiteral("ALL LATER LOW OTHER"));
    // LATER becomes active later than the hours ahead
    environment.setNow(now.addSecs(qint64{ 6 - GeoZoneMapFilter::scheduleHours - 1 } * 3600));
    settings->scheduleFilter()->setRawValue(GeoZoneMapFilter::AnyTime);
    settings->scheduleFilter()->setRawValue(GeoZoneMapFilter::ActiveSoon);
    QCOMPARE(shown(), QStringLiteral("ALL LOW OTHER"));

    // At the planned flight time, from its start to its estimated end, or now without planned start
    settings->scheduleFilter()->setRawValue(GeoZoneMapFilter::ActiveAtFlightTime);
    QCOMPARE(shown(), QStringLiteral("ALL LOW OTHER"));
    manager.planning()->setPlannedStart(now.addSecs(7 * 3600));
    QCOMPARE(shown(), QStringLiteral("ALL LATER LOW OTHER"));
    manager.planning()->setPlannedStart(now.addSecs(4 * 3600));
    QCOMPARE(shown(), QStringLiteral("ALL LOW OTHER"));
    manager.planning()->setPlannedEnd(now.addSecs((6 * 3600) - GeoZoneMapFilter::scheduleToleranceSecs + 60));
    QCOMPARE(shown(), QStringLiteral("ALL LATER LOW OTHER"));
    manager.planning()->clearPlannedStart();
    manager.planning()->setPlannedEnd(QDateTime());
    QCOMPARE(shown(), QStringLiteral("ALL LOW OTHER"));
    settings->scheduleFilter()->setRawValue(GeoZoneMapFilter::ActiveSoon);

    // Sources, with the other filters
    settings->mapSource()->setRawValue(zone(QStringLiteral("OTHER"))->sourceKey());
    QCOMPARE(shown(), QStringLiteral("OTHER"));
    QCOMPARE(model.filteredZoneCount(), 4);
    settings->mapSource()->setRawValue(zone(QStringLiteral("LOW"))->sourceKey());
    QCOMPARE(shown(), QStringLiteral("ALL LOW"));
    display->setRestrictionVisible(GeoZone::Conditional, false);
    QCOMPARE(shown(), QStringLiteral("LOW"));
    // The key of a source which isn't loaded doesn't hide the others
    settings->mapSource()->setRawValue(QStringLiteral("removed.json"));
    QCOMPARE(shown(), QStringLiteral("LOW OTHER"));

    display->resetFilters();
    QCOMPARE(shown(), QStringLiteral("ALL HIGH LATER LOW OTHER"));
    QCOMPARE(model.filteredZoneCount(), 0);

    // The altitudes of a mission, read again when its items change
    settings->altitudeFilter()->setRawValue(true);
    PlanMasterController masterController;
    masterController.setFlyView(false);
    masterController.start();
    MissionController *const missionController = masterController.missionController();
    const QGeoCoordinate home = center.atDistanceAndAzimuth(1000, 270);
    missionController->visualItems()->value<MissionSettingsItem*>(0)->setInitialHomePosition(QGeoCoordinate(home.latitude(), home.longitude(), 400));
    QList<SimpleMissionItem*> items;
    for (const QGeoCoordinate &coordinate : { home, center.atDistanceAndAzimuth(1000, 90) }) {
        items.append(qobject_cast<SimpleMissionItem*>(missionController->insertSimpleMissionItem(coordinate, missionController->visualItems()->count())));
        items.last()->altitude()->setRawValue(50);
    }
    model.setMissionController(missionController);
    // The home altitude may change to the terrain elevation once it is received, the zone floor is far above both
    QTRY_VERIFY_WITH_TIMEOUT(model.altitudeRange().isValid(), 5000);
    QCOMPARE(qRound(model.altitudeRange().maxAMSL - model.altitudeRange().minAMSL), 0);
    QCOMPARE(shown(), QStringLiteral("ALL LATER LOW OTHER"));
    items.last()->altitude()->setRawValue(1450);
    QTRY_COMPARE_WITH_TIMEOUT(shown(), QStringLiteral("ALL HIGH LATER LOW OTHER"), 5000);
    QCOMPARE(qRound(model.altitudeRange().maxAMSL - model.altitudeRange().minAMSL), 1400);
    model.setMissionController(nullptr);
}

GEOZONE_TEST(GeoZonePlanningTest)
