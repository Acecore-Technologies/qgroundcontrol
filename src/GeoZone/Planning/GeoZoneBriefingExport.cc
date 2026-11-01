/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneBriefingExport.h"
#include "AppSettings.h"
#include "GeoZoneAuthorisations.h"
#include "GeoZoneManager.h"
#include "GeoZoneMissionCheck.h"
#include "GeoZoneMissionPath.h"
#include "GeoZonePlanning.h"
#include "GeoZoneSource.h"
#include "GeoZoneSourceRepository.h"
#include "MissionController.h"
#include "PlanMasterController.h"
#include "QmlObjectListModel.h"
#include "SettingsManager.h"
#include "Vehicle.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtGui/QAbstractTextDocumentLayout>
#include <QtGui/QFontMetricsF>
#include <QtGui/QPainter>
#include <QtGui/QPdfWriter>
#include <QtGui/QTextDocument>

#include <algorithm>
#include <cmath>

namespace {

using Briefing = GeoZoneBriefing;

/// Translation context of the helpers
class Text
{
    Q_DECLARE_TR_FUNCTIONS(GeoZoneBriefingExport)
};

QJsonValue jsonNumber(double value)
{
    return std::isfinite(value) ? QJsonValue(value) : QJsonValue(QJsonValue::Null);
}

QJsonValue jsonTime(const QDateTime &dateTime)
{
    return dateTime.isValid() ? QJsonValue(dateTime.toUTC().toString(Qt::ISODate)) : QJsonValue(QJsonValue::Null);
}

QJsonValue jsonCoordinate(const QGeoCoordinate &coordinate)
{
    if (!coordinate.isValid()) {
        return QJsonValue(QJsonValue::Null);
    }
    QJsonObject json;
    json.insert(QStringLiteral("lat"), coordinate.latitude());
    json.insert(QStringLiteral("lon"), coordinate.longitude());
    return json;
}

QJsonValue jsonItem(int sequenceNumber)
{
    return (sequenceNumber >= 0) ? QJsonValue(sequenceNumber) : QJsonValue(QJsonValue::Null);
}

QString escaped(const QString &text)
{
    return text.toHtmlEscaped().replace(QChar(u'\n'), QStringLiteral("<br>"));
}

QString dateTimeText(const QDateTime &dateTime)
{
    if (!dateTime.isValid()) {
        return Text::tr("unknown");
    }
    const QDateTime local = dateTime.toLocalTime();
    const QDateTime utc = dateTime.toUTC();
    return Text::tr("%1 (%2 UTC)").arg(local.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                                 utc.toString((utc.date() == local.date()) ? QStringLiteral("HH:mm") : QStringLiteral("yyyy-MM-dd HH:mm")));
}

/// Local time of the day, with the date when the flight started another day
QString timeText(const QDateTime &dateTime, const QDateTime &start)
{
    const QDateTime local = dateTime.toLocalTime();
    return (start.isValid() && (local.date() == start.toLocalTime().date())) ? local.toString(QStringLiteral("HH:mm")) :
                                                                                local.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString altitudeText(double meters)
{
    if (!std::isfinite(meters)) {
        return Text::tr("unknown");
    }
    return Text::tr("%1 m (%2 ft) AMSL").arg(std::lround(meters)).arg(std::lround(meters / 0.3048));
}

QString distanceText(double meters)
{
    if (!std::isfinite(meters)) {
        return Text::tr("unknown");
    }
    return (meters < 1000) ? Text::tr("%1 m").arg(std::lround(meters)) : Text::tr("%1 km").arg(QString::number(meters / 1000, 'f', 1));
}

QString durationText(double seconds)
{
    if (!std::isfinite(seconds) || (seconds < 0)) {
        return Text::tr("unknown");
    }
    const long total = std::lround(seconds);
    const long hours = total / 3600;
    const long minutes = (total % 3600) / 60;
    if (hours > 0) {
        return Text::tr("%1 h %2 min").arg(hours).arg(minutes, 2, 10, QChar(u'0'));
    }
    return Text::tr("%1 min %2 s").arg(minutes).arg(total % 60, 2, 10, QChar(u'0'));
}

QString hashText(const QByteArray &hash)
{
    return hash.isEmpty() ? Text::tr("none") : QString::fromLatin1(hash.toHex().left(16));
}

QString restrictionColor(GeoZone::Restriction restriction)
{
    switch (restriction) {
    case GeoZone::Prohibited:
        return QStringLiteral("#c62828");
    case GeoZone::ReqAuthorisation:
        return QStringLiteral("#e65100");
    case GeoZone::Conditional:
        return QStringLiteral("#9e7b00");
    case GeoZone::NoRestriction:
        return QStringLiteral("#2e7d32");
    case GeoZone::USpace:
        return QStringLiteral("#1565c0");
    case GeoZone::Unknown:
        break;
    }
    return QStringLiteral("#616161");
}

QJsonArray jsonApproximations(GeoZonePathEvaluator::Approximations approximations)
{
    return QJsonArray::fromStringList(GeoZonePathEvaluator::approximationCodes(approximations));
}

QString row(const QString &header, const QString &html)
{
    return html.isEmpty() ? QString() : QStringLiteral("<tr><th>%1</th><td>%2</td></tr>\n").arg(header.toHtmlEscaped(), html);
}

QString crossingHtml(const Briefing::Crossing &crossing, const Briefing &briefing)
{
    const QDateTime start = briefing.mission.plannedStart;
    QString entry = (crossing.entryItem >= 0) ? Text::tr("Enters after item %1, %2 along the mission").arg(crossing.entryItem).arg(distanceText(crossing.entryDistance)) :
                                                Text::tr("Enters %1 along the mission").arg(distanceText(crossing.entryDistance));
    if (crossing.entryTime.isValid()) {
        entry = Text::tr("%1, at about %2").arg(entry, timeText(crossing.entryTime, start));
    }
    QString exit = (crossing.exitItem >= 0) ? Text::tr("leaves after item %1, %2 along the mission").arg(crossing.exitItem).arg(distanceText(crossing.exitDistance)) :
                                              Text::tr("leaves %1 along the mission").arg(distanceText(crossing.exitDistance));
    if (crossing.exitTime.isValid()) {
        exit = Text::tr("%1, at about %2").arg(exit, timeText(crossing.exitTime, start));
    }
    QStringList parts = { Text::tr("%1; %2.").arg(entry, exit) };
    if (std::isfinite(crossing.pathAltitudeMin)) {
        parts.append((std::lround(crossing.pathAltitudeMin) == std::lround(crossing.pathAltitudeMax)) ?
                         Text::tr("Flies at %1 inside.").arg(altitudeText(crossing.pathAltitudeMin)) :
                         Text::tr("Flies %1 to %2 inside.").arg(altitudeText(crossing.pathAltitudeMin), altitudeText(crossing.pathAltitudeMax)));
    }
    parts.append(Text::tr("Zone at the entry from %1 to %2.").arg(std::isfinite(crossing.zoneFloor) ? altitudeText(crossing.zoneFloor) : Text::tr("the ground or unknown"),
                                                             std::isfinite(crossing.zoneCeiling) ? altitudeText(crossing.zoneCeiling) : Text::tr("no ceiling or unknown")));
    if (!crossing.active) {
        parts.append(Text::tr("The zone doesn't apply then."));
    }
    QString text = parts.join(QChar(u' '));
    if (!crossing.path.isEmpty()) {
        QString label = crossing.path;
        label[0] = label[0].toUpper();
        text = Text::tr("%1: %2").arg(label, text);
    }
    return escaped(text);
}

QString whenHtml(const Briefing::Zone &zone, const Briefing &briefing)
{
    QStringList lines;
    if (!briefing.mission.plannedStart.isValid()) {
        lines.append(zone.activeDuringFlight ? Text::tr("No planned start, active now.") : Text::tr("No planned start, not active now."));
    } else if (zone.activePeriods.isEmpty()) {
        lines.append(Text::tr("Not active during the planned flight."));
    } else if (zone.schedule.isEmpty()) {
        lines.append(Text::tr("Permanent."));
    } else {
        QStringList periods;
        for (const Briefing::Interval &period : zone.activePeriods) {
            periods.append(QStringLiteral("%1\u2013%2").arg(timeText(period.start, briefing.mission.plannedStart), timeText(period.end, briefing.mission.plannedStart)));
        }
        lines.append(Text::tr("Active during the flight: %1 (local time, flight \u00b1%n min).", nullptr, briefing.scheduleTolerance / 60).arg(periods.join(QStringLiteral(", "))));
    }
    if (!zone.schedule.isEmpty()) {
        lines.append(Text::tr("Schedule: %1").arg(zone.schedule));
    }
    return escaped(lines.join(QChar(u'\n')));
}

QString zoneHtml(const Briefing::Zone &zone, const Briefing &briefing)
{
    QString html;
    const QString color = restrictionColor(zone.restriction);
    QString title = zone.displayName().toHtmlEscaped();
    if (!zone.identifier.isEmpty() && (zone.identifier != zone.displayName())) {
        title += QStringLiteral(" <small>(%1)</small>").arg(zone.identifier.toHtmlEscaped());
    }
    html += QStringLiteral("<table class=\"zone\" width=\"100%\" cellspacing=\"0\" cellpadding=\"4\">\n");
    html += QStringLiteral("<tr><td colspan=\"2\" class=\"zonetitle\" bgcolor=\"%1\"><font color=\"#ffffff\"><b>%2</b> \u2014 %3</font></td></tr>\n")
                .arg(color, title, GeoZone::restrictionToString(zone.restriction).toHtmlEscaped());

    QString restriction = GeoZone::restrictionToString(zone.restriction);
    if (!zone.restrictionCode.isEmpty()) {
        restriction = Text::tr("%1 (published as %2)").arg(restriction, zone.restrictionCode);
    }
    if (!zone.reasons.isEmpty()) {
        restriction = Text::tr("%1. Reason: %2").arg(restriction, zone.reasons);
    }
    html += row(Text::tr("Restriction"), escaped(restriction));
    if (!zone.country.isEmpty()) {
        html += row(Text::tr("Country"), escaped(zone.country));
    }

    QStringList where;
    for (const Briefing::Crossing &crossing : zone.crossings) {
        where.append(QStringLiteral("<li>%1</li>").arg(crossingHtml(crossing, briefing)));
    }
    if (zone.inArea) {
        where.append(QStringLiteral("<li>%1</li>").arg(escaped(Text::tr("A pattern area of the mission overlaps the zone."))));
    }
    for (const QString &label : zone.contingencies) {
        QString text = Text::tr("%1 would enter the zone.").arg(label);
        text[0] = text[0].toUpper();
        where.append(QStringLiteral("<li>%1</li>").arg(escaped(text)));
    }
    if (zone.involvement == Briefing::Involvement::Nearby) {
        QString text;
        if (zone.passed == GeoZone::VerticalPosition::Above) {
            text = Text::tr("The flight path passes above the zone.");
        } else if (zone.passed == GeoZone::VerticalPosition::Below) {
            text = Text::tr("The flight path passes below the zone.");
        } else {
            text = Text::tr("%1 from the flight path.").arg(distanceText(zone.distance));
        }
        where.append(QStringLiteral("<li>%1</li>").arg(escaped(text)));
    }
    if (!where.isEmpty()) {
        html += row(Text::tr("Where"), QStringLiteral("<ul>%1</ul>").arg(where.join(QString())));
    }
    html += row(Text::tr("When"), whenHtml(zone, briefing));
    html += row(Text::tr("Vertical limits"), escaped(zone.verticalLimits));
    html += row(Text::tr("Approximate"), escaped(GeoZonePathEvaluator::approximationTexts(zone.approximations).join(QChar(u'\n'))));
    html += row(Text::tr("Authority"), escaped(zone.authorities.join(QChar(u'\n'))));
    html += row(Text::tr("Message"), escaped(zone.message));
    html += row(Text::tr("Conditions"), escaped(zone.conditions));
    html += row(Text::tr("Other reasons"), escaped(zone.otherReasonInfo));
    html += row(Text::tr("U-space class"), escaped(zone.uSpaceClass));
    html += row(Text::tr("Exemptions"), escaped(zone.regulationExemption));
    if (zone.authorisation) {
        const Briefing::Authorisation &authorisation = *zone.authorisation;
        QString text = authorisation.permit.isEmpty() ? authorisation.status : Text::tr("Permit %1, %2").arg(authorisation.permit, authorisation.status);
        text = authorisation.coversFlight ? Text::tr("%1. Covers the flight.").arg(text) : Text::tr("%1. Does not cover the flight.").arg(text);
        html += row(Text::tr("Authorisation"), escaped(text));
    } else if (GeoZonePolicy::isRestricted(zone.restriction) && (zone.restriction != GeoZone::Prohibited)) {
        html += row(Text::tr("Authorisation"), escaped(Text::tr("None recorded.")));
    }
    QString source = zone.sourceKey;
    for (const GeoZoneSourceVersion &version : briefing.sources) {
        if ((version.key == zone.sourceKey) && !version.name.isEmpty()) {
            source = version.name;
        }
    }
    html += row(Text::tr("Data"), escaped(Text::tr("%1, zone hash %2").arg(source, hashText(zone.contentHash))));
    html += QStringLiteral("</table>\n<p></p>\n");
    return html;
}

QString style(bool forPrint)
{
    if (forPrint) {
        // The subset QTextDocument supports
        return QStringLiteral(
            "body { font-size: 9pt; }"
            "h1 { font-size: 15pt; }"
            "h2 { font-size: 12pt; margin-top: 12px; }"
            "table { border-collapse: collapse; }"
            "th { text-align: left; vertical-align: top; font-weight: bold; width: 25%; border-bottom: 1px solid #cccccc; }"
            "td { vertical-align: top; border-bottom: 1px solid #cccccc; }"
            "table.zone { border: 1px solid #999999; }"
            "ul { margin-left: 0px; -qt-list-indent: 1; }"
            ".meta { color: #555555; }");
    }
    return QStringLiteral(
        "body{font-family:-apple-system,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;color:#111;line-height:1.4;margin:0 auto;"
        "max-width:60em;padding:1em}"
        "h1{font-size:1.5em;margin:0 0 .3em}"
        "h2{font-size:1.2em;border-bottom:2px solid #333;margin-top:1.6em;padding-bottom:.1em}"
        "table{border-collapse:collapse;width:100%;margin:.4em 0}"
        "th,td{text-align:left;vertical-align:top;padding:.3em .5em;border-bottom:1px solid #ddd}"
        "th{width:11em;color:#444}"
        "table.zone{border:1px solid #999;margin:1em 0;page-break-inside:avoid;break-inside:avoid}"
        "td.zonetitle{padding:.4em .6em;border:none}"
        "ul{margin:0;padding-left:1.2em}"
        ".meta{color:#555;font-size:.9em}"
        "code{font-size:.9em;word-break:break-all}"
        "@media (max-width:40em){body{padding:.5em}th{display:block;width:auto;border:none;padding-bottom:0;font-size:.85em}"
        "td{display:block}tr{display:block;border-bottom:1px solid #ddd}td.zonetitle{display:block}}"
        "@media print{body{max-width:none;padding:0}h2{page-break-after:avoid;break-after:avoid}}");
}

QString returnAutopilotText(GeoZoneReturnModel::Autopilot autopilot)
{
    switch (autopilot) {
    case GeoZoneReturnModel::Autopilot::PX4:
        return QStringLiteral("PX4");
    case GeoZoneReturnModel::Autopilot::ArduCopter:
        return QStringLiteral("ArduCopter");
    case GeoZoneReturnModel::Autopilot::ArduPlane:
        return QStringLiteral("ArduPlane");
    case GeoZoneReturnModel::Autopilot::Unknown:
        break;
    }
    return Text::tr("a generic autopilot");
}

} // namespace

GeoZoneBriefingExport::GeoZoneBriefingExport(QObject *parent)
    : GeoZoneBriefingExport(GeoZoneManager::instance(), parent)
{
}

GeoZoneBriefingExport::GeoZoneBriefingExport(GeoZoneManager *manager, QObject *parent)
    : QObject(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
{
}

MissionController *GeoZoneBriefingExport::missionController() const
{
    return _missionController;
}

void GeoZoneBriefingExport::setMissionController(MissionController *missionController)
{
    if (missionController != _missionController) {
        _missionController = missionController;
        emit missionControllerChanged();
    }
}

QStringList GeoZoneBriefingExport::nameFilters()
{
    return { tr("PDF document (*.pdf)"), tr("Web page (*.html)"), tr("JSON data (*.json)") };
}

void GeoZoneBriefingExport::_setErrorString(const QString &errorString)
{
    if (errorString != _errorString) {
        _errorString = errorString;
        emit errorStringChanged();
    }
}

QString GeoZoneBriefingExport::defaultFolder() const
{
    const PlanMasterController *const masterController = _missionController ? _missionController->masterController() : nullptr;
    if (masterController && !masterController->currentPlanFile().isEmpty()) {
        return QFileInfo(masterController->currentPlanFile()).absolutePath();
    }
    return SettingsManager::instance()->appSettings()->missionSavePath();
}

QString GeoZoneBriefingExport::defaultFileName() const
{
    const PlanMasterController *const masterController = _missionController ? _missionController->masterController() : nullptr;
    if (masterController && !masterController->currentPlanFile().isEmpty()) {
        return QStringLiteral("%1-briefing.pdf").arg(QFileInfo(masterController->currentPlanFile()).completeBaseName());
    }
    return QStringLiteral("geozone-briefing.pdf");
}

std::optional<GeoZoneBriefingExport::Format> GeoZoneBriefingExport::formatOf(const QString &fileName)
{
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (suffix == QStringLiteral("json")) {
        return Format::Json;
    }
    if ((suffix == QStringLiteral("html")) || (suffix == QStringLiteral("htm"))) {
        return Format::Html;
    }
    if (suffix == QStringLiteral("pdf")) {
        return Format::Pdf;
    }
    return std::nullopt;
}

bool GeoZoneBriefingExport::exportBriefing(const QString &fileName)
{
    if (fileName.isEmpty()) {
        _setErrorString(tr("No file name"));
        return false;
    }
    QString path = fileName;
    std::optional<Format> format = formatOf(path);
    if (!format) {
        path += QStringLiteral(".pdf");
        format = Format::Pdf;
    }
    QString errorString;
    const bool written = write(briefing(), path, *format, errorString);
    _setErrorString(written ? QString() : errorString);
    return written;
}

GeoZoneBriefing GeoZoneBriefingExport::briefing() const
{
    MissionController *const missionController = _missionController;
    const GeoZoneMissionPath path = GeoZoneMissionPath::fromMission(missionController, true);
    const auto [start, end] = GeoZoneMissionCheck::plannedFlightTimes(_manager, missionController);
    const QDateTime flightEnd = path.flightEnd(start, end);

    GeoZoneBriefing::Input input;
    GeoZoneBriefing::Mission &mission = input.mission;
    PlanMasterController *const masterController = missionController ? missionController->masterController() : nullptr;
    mission.planFile = masterController ? masterController->currentPlanFile() : QString();
    mission.name = mission.planFile.isEmpty() ? tr("Untitled plan") : QFileInfo(mission.planFile).completeBaseName();
    Vehicle *const vehicle = masterController ? masterController->managerVehicle() : nullptr;
    if (vehicle) {
        mission.vehicle = vehicle->vehicleTypeString();
        mission.firmware = vehicle->firmwareTypeString();
        if (!vehicle->isOfflineEditingVehicle() && (vehicle->firmwareMajorVersion() != Vehicle::versionNotSetValue)) {
            mission.firmware += QStringLiteral(" %1.%2.%3").arg(vehicle->firmwareMajorVersion()).arg(vehicle->firmwareMinorVersion()).arg(vehicle->firmwarePatchVersion());
        }
    }
    mission.plannedStart = start;
    mission.plannedEnd = flightEnd;
    mission.distance = path.flightDistance();
    const double missionTime = missionController ? missionController->missionTime() : qQNaN();
    if (start.isValid()) {
        mission.duration = static_cast<double>(start.secsTo(flightEnd));
    } else if ((missionTime > 0) && (path.totalDistance() > 0)) {
        mission.duration = missionTime * (path.flightDistance() / path.totalDistance());
    }
    if (missionController) {
        mission.home = missionController->plannedHomePosition();
        const QmlObjectListModel *const visualItems = missionController->visualItems();
        mission.itemCount = visualItems ? (std::max)(visualItems->count() - 1, 0) : 0;
        for (int i = 0; visualItems && (i < visualItems->count()); i++) {
            const double distance = path.itemFlightDistance(i);
            const int sequenceNumber = path.sequenceNumber(i);
            if (!qIsNaN(distance) && (sequenceNumber >= 0)) {
                input.items.append(GeoZoneBriefing::Item{ sequenceNumber, distance });
            }
        }
        std::stable_sort(input.items.begin(), input.items.end(), [](const GeoZoneBriefing::Item &a, const GeoZoneBriefing::Item &b) { return a.distance < b.distance; });
    }
    for (const GeoZoneMissionPath::Path &missionPath : path.paths()) {
        for (const GeoZone::Position &position : missionPath.positions) {
            if (!qIsNaN(position.altitudeAMSL) && (qIsNaN(mission.maxAltitudeAMSL) || (position.altitudeAMSL > mission.maxAltitudeAMSL))) {
                mission.maxAltitudeAMSL = position.altitudeAMSL;
            }
        }
    }
    if (mission.home.isValid() && !qIsNaN(mission.home.altitude())) {
        mission.maxAltitudeAboveHome = mission.maxAltitudeAMSL - mission.home.altitude();
    }

    const GeoZoneReturnModel::Parameters parameters = GeoZoneMissionPath::returnParameters(vehicle);
    mission.returnAssumptions.append(parameters.assumed ? tr("Returns are flown the way %1 does with the firmware's default parameters, the vehicle's being unknown.").arg(returnAutopilotText(parameters.autopilot)) :
                                                          tr("Returns are flown the way %1 does with the vehicle's parameters.").arg(returnAutopilotText(parameters.autopilot)));
    mission.returnAssumptions.append(parameters.altitudeKnown() ? tr("Return altitude parameter: %1 m.").arg(std::lround(parameters.returnAltitude)) : tr("Return altitude unknown."));
    switch (parameters.breachAction) {
    case GeoZoneReturnModel::BreachAction::None:
        mission.returnAssumptions.append(tr("After a fence breach the vehicle doesn't return."));
        break;
    case GeoZoneReturnModel::BreachAction::Return:
        mission.returnAssumptions.append(tr("After a fence breach the vehicle returns."));
        break;
    case GeoZoneReturnModel::BreachAction::ReturnPoint:
        mission.returnAssumptions.append(tr("After a fence breach the vehicle flies to the breach return point."));
        break;
    }
    mission.returnAssumptions.append(tr("Returns checked from %n position(s) along the mission.", nullptr, static_cast<int>(path.contingencyStarts().count())));

    input.paths = path.evaluatorPaths(start, end);
    for (const GeoZoneMissionPath::Path &missionPath : path.paths()) {
        input.pathDistances.append(missionPath.distances);
    }
    input.areas = path.areas();
    input.contingencyPaths = path.contingencyEvaluatorPaths(start, end);
    input.groupContingencies = [&path](const GeoZonePathEvaluator::Result &result) { return GeoZoneMissionCheck::groupContingencies(path, result); };
    input.start = start;
    input.end = flightEnd;
    input.authorisations = _manager->planning()->authorisations();
    const QList<const GeoZoneSource*> sources = _manager->repository()->sourceList();
    input.sources = GeoZoneSourceVersion::covering(sources, path.box());

    GeoZoneBriefing briefing = GeoZoneBriefing::build(_manager->catalog(), input);
    // Nearby zones may be of sources just outside the paths
    for (const GeoZoneBriefing::Zone &zone : std::as_const(briefing.zones)) {
        const bool listed = std::any_of(briefing.sources.cbegin(), briefing.sources.cend(), [&zone](const GeoZoneSourceVersion &source) { return source.key == zone.sourceKey; });
        if (!listed) {
            if (const GeoZoneSource *const source = _manager->source(zone.sourceKey)) {
                briefing.sources.append(GeoZoneSourceVersion::fromSource(source));
            }
        }
    }
    return briefing;
}

QJsonObject GeoZoneBriefingExport::toJson(const GeoZoneBriefing &briefing)
{
    const GeoZoneBriefing::Mission &mission = briefing.mission;
    QJsonObject missionJson;
    missionJson.insert(QStringLiteral("name"), mission.name);
    missionJson.insert(QStringLiteral("planFile"), mission.planFile);
    missionJson.insert(QStringLiteral("vehicle"), mission.vehicle);
    missionJson.insert(QStringLiteral("firmware"), mission.firmware);
    missionJson.insert(QStringLiteral("plannedStart"), jsonTime(mission.plannedStart));
    missionJson.insert(QStringLiteral("plannedEnd"), jsonTime(mission.plannedEnd));
    missionJson.insert(QStringLiteral("durationSeconds"), jsonNumber(mission.duration));
    missionJson.insert(QStringLiteral("distanceMeters"), jsonNumber(mission.distance));
    missionJson.insert(QStringLiteral("maxAltitudeAMSL"), jsonNumber(mission.maxAltitudeAMSL));
    missionJson.insert(QStringLiteral("maxAltitudeAboveHome"), jsonNumber(mission.maxAltitudeAboveHome));
    QJsonValue home = jsonCoordinate(mission.home);
    if (home.isObject()) {
        QJsonObject homeJson = home.toObject();
        homeJson.insert(QStringLiteral("altitudeAMSL"), jsonNumber(mission.home.altitude()));
        home = homeJson;
    }
    missionJson.insert(QStringLiteral("home"), home);
    missionJson.insert(QStringLiteral("itemCount"), mission.itemCount);
    missionJson.insert(QStringLiteral("returnAssumptions"), QJsonArray::fromStringList(mission.returnAssumptions));

    QJsonArray zones;
    for (const GeoZoneBriefing::Zone &zone : briefing.zones) {
        QJsonObject json;
        json.insert(QStringLiteral("involvement"), GeoZoneBriefing::involvementCode(zone.involvement));
        json.insert(QStringLiteral("key"), zone.key);
        json.insert(QStringLiteral("identifier"), zone.identifier);
        json.insert(QStringLiteral("country"), zone.country);
        json.insert(QStringLiteral("name"), zone.name);
        json.insert(QStringLiteral("restriction"), GeoZoneBriefing::restrictionCode(zone.restriction));
        json.insert(QStringLiteral("restrictionCode"), zone.restrictionCode);
        json.insert(QStringLiteral("restrictionText"), GeoZone::restrictionToString(zone.restriction));
        json.insert(QStringLiteral("reasons"), zone.reasons);
        json.insert(QStringLiteral("verticalLimits"), zone.verticalLimits);
        json.insert(QStringLiteral("schedule"), zone.schedule);
        json.insert(QStringLiteral("activeDuringFlight"), zone.activeDuringFlight);
        QJsonArray periods;
        for (const GeoZoneBriefing::Interval &period : zone.activePeriods) {
            periods.append(QJsonObject{ { QStringLiteral("start"), jsonTime(period.start) }, { QStringLiteral("end"), jsonTime(period.end) } });
        }
        json.insert(QStringLiteral("activePeriods"), periods);
        QJsonArray crossings;
        for (const GeoZoneBriefing::Crossing &crossing : zone.crossings) {
            QJsonObject crossingJson;
            crossingJson.insert(QStringLiteral("path"), crossing.path);
            crossingJson.insert(QStringLiteral("entryItem"), jsonItem(crossing.entryItem));
            crossingJson.insert(QStringLiteral("exitItem"), jsonItem(crossing.exitItem));
            crossingJson.insert(QStringLiteral("entryDistance"), jsonNumber(crossing.entryDistance));
            crossingJson.insert(QStringLiteral("exitDistance"), jsonNumber(crossing.exitDistance));
            crossingJson.insert(QStringLiteral("entry"), jsonCoordinate(crossing.entry));
            crossingJson.insert(QStringLiteral("exit"), jsonCoordinate(crossing.exit));
            crossingJson.insert(QStringLiteral("entryTime"), jsonTime(crossing.entryTime));
            crossingJson.insert(QStringLiteral("exitTime"), jsonTime(crossing.exitTime));
            crossingJson.insert(QStringLiteral("pathAltitudeMin"), jsonNumber(crossing.pathAltitudeMin));
            crossingJson.insert(QStringLiteral("pathAltitudeMax"), jsonNumber(crossing.pathAltitudeMax));
            crossingJson.insert(QStringLiteral("zoneFloor"), jsonNumber(crossing.zoneFloor));
            crossingJson.insert(QStringLiteral("zoneCeiling"), jsonNumber(crossing.zoneCeiling));
            crossingJson.insert(QStringLiteral("active"), crossing.active);
            crossingJson.insert(QStringLiteral("approximations"), jsonApproximations(crossing.approximations));
            crossings.append(crossingJson);
        }
        json.insert(QStringLiteral("crossings"), crossings);
        json.insert(QStringLiteral("inArea"), zone.inArea);
        json.insert(QStringLiteral("contingencies"), QJsonArray::fromStringList(zone.contingencies));
        json.insert(QStringLiteral("distance"), jsonNumber(zone.distance));
        QJsonValue passed(QJsonValue::Null);
        if (zone.passed == GeoZone::VerticalPosition::Above) {
            passed = QStringLiteral("above");
        } else if (zone.passed == GeoZone::VerticalPosition::Below) {
            passed = QStringLiteral("below");
        }
        json.insert(QStringLiteral("passed"), passed);
        json.insert(QStringLiteral("authorities"), QJsonArray::fromStringList(zone.authorities));
        json.insert(QStringLiteral("message"), zone.message);
        json.insert(QStringLiteral("conditions"), zone.conditions);
        json.insert(QStringLiteral("otherReasonInfo"), zone.otherReasonInfo);
        json.insert(QStringLiteral("uSpaceClass"), zone.uSpaceClass);
        json.insert(QStringLiteral("regulationExemption"), zone.regulationExemption);
        QJsonValue authorisation(QJsonValue::Null);
        if (zone.authorisation) {
            authorisation = QJsonObject{
                { QStringLiteral("permit"), zone.authorisation->permit },
                { QStringLiteral("validFrom"), jsonTime(zone.authorisation->validFrom) },
                { QStringLiteral("validTo"), jsonTime(zone.authorisation->validTo) },
                { QStringLiteral("status"), zone.authorisation->status },
                { QStringLiteral("coversFlight"), zone.authorisation->coversFlight },
            };
        }
        json.insert(QStringLiteral("authorisation"), authorisation);
        json.insert(QStringLiteral("source"), zone.sourceKey);
        json.insert(QStringLiteral("contentHash"), QString::fromLatin1(zone.contentHash.toHex()));
        json.insert(QStringLiteral("approximations"), jsonApproximations(zone.approximations));
        zones.append(json);
    }

    QJsonArray sources;
    for (const GeoZoneSourceVersion &source : briefing.sources) {
        sources.append(source.toJson());
    }

    QJsonObject json;
    json.insert(QStringLiteral("schema"), QString::fromLatin1(jsonSchema));
    json.insert(QStringLiteral("version"), jsonVersion);
    json.insert(QStringLiteral("generated"), jsonTime(briefing.generatedAt));
    json.insert(QStringLiteral("generator"), QStringLiteral("%1 %2").arg(QCoreApplication::applicationName(), QCoreApplication::applicationVersion()).trimmed());
    json.insert(QStringLiteral("scheduleToleranceSeconds"), briefing.scheduleTolerance);
    json.insert(QStringLiteral("nearbyDistanceMeters"), briefing.nearbyDistance);
    json.insert(QStringLiteral("approximations"), jsonApproximations(briefing.approximations));
    json.insert(QStringLiteral("mission"), missionJson);
    json.insert(QStringLiteral("zones"), zones);
    json.insert(QStringLiteral("sources"), sources);
    return json;
}

QString GeoZoneBriefingExport::toHtml(const GeoZoneBriefing &briefing, bool forPrint)
{
    const GeoZoneBriefing::Mission &mission = briefing.mission;
    const QString title = tr("Geozone briefing: %1").arg(mission.name);
    QString html;
    html += QStringLiteral("<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n"
                           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n");
    html += QStringLiteral("<title>%1</title>\n<style>%2</style>\n</head><body>\n").arg(title.toHtmlEscaped(), style(forPrint));
    html += QStringLiteral("<h1>%1</h1>\n").arg(title.toHtmlEscaped());
    html += QStringLiteral("<p class=\"meta\">%1</p>\n")
                .arg(escaped(tr("Generated %1 by QGroundControl from the geozone data loaded in it. It doesn't replace the official "
                                "aeronautical information and the authorisations the flight requires.").arg(dateTimeText(briefing.generatedAt))));

    // Mission
    const QList<const GeoZoneBriefing::Zone*> entered = briefing.zonesWith(GeoZoneBriefing::Involvement::Entered);
    const QList<const GeoZoneBriefing::Zone*> contingency = briefing.zonesWith(GeoZoneBriefing::Involvement::Contingency);
    const QList<const GeoZoneBriefing::Zone*> nearby = briefing.zonesWith(GeoZoneBriefing::Involvement::Nearby);
    const qsizetype restricted = std::count_if(entered.cbegin(), entered.cend(), [](const GeoZoneBriefing::Zone *zone) {
        return GeoZonePolicy::isRestricted(zone->restriction) && zone->activeDuringFlight && !(zone->authorisation && zone->authorisation->coversFlight);
    });
    html += QStringLiteral("<h2>%1</h2>\n<table cellspacing=\"0\" cellpadding=\"4\" width=\"100%\">\n").arg(tr("Mission").toHtmlEscaped());
    html += row(tr("Plan"), escaped(mission.planFile.isEmpty() ? mission.name : mission.planFile));
    QStringList vehicle = { mission.vehicle, mission.firmware };
    vehicle.removeAll(QString());
    html += row(tr("Vehicle"), escaped(vehicle.isEmpty() ? tr("unknown") : vehicle.join(QStringLiteral(", "))));
    html += row(tr("Planned start"), escaped(mission.plannedStart.isValid() ? dateTimeText(mission.plannedStart) :
                                                                              tr("None, the zones are checked whatever their schedule")));
    if (mission.plannedStart.isValid()) {
        html += row(tr("Estimated end"), escaped(dateTimeText(mission.plannedEnd)));
    }
    html += row(tr("Duration"), escaped(durationText(mission.duration)));
    html += row(tr("Distance"), escaped(distanceText(mission.distance)));
    QString maxAltitude = altitudeText(mission.maxAltitudeAMSL);
    if (std::isfinite(mission.maxAltitudeAboveHome)) {
        maxAltitude = tr("%1, %2 m above home").arg(maxAltitude).arg(std::lround(mission.maxAltitudeAboveHome));
    }
    html += row(tr("Highest altitude"), escaped(maxAltitude));
    if (mission.home.isValid()) {
        html += row(tr("Home"), escaped(tr("%1, %2, %3").arg(QString::number(mission.home.latitude(), 'f', 6), QString::number(mission.home.longitude(), 'f', 6),
                                                              altitudeText(mission.home.altitude()))));
    }
    html += row(tr("Items"), escaped(QString::number(mission.itemCount)));
    html += row(tr("Returns"), escaped(mission.returnAssumptions.join(QChar(u'\n'))));
    QStringList summary;
    summary.append(tr("The flight path enters %n zone(s)", nullptr, static_cast<int>(entered.count())));
    if (restricted > 0) {
        summary.append(tr("%n restricted while active and not authorised", nullptr, static_cast<int>(restricted)));
    }
    summary.append(tr("returns would enter %n more", nullptr, static_cast<int>(contingency.count())));
    summary.append(tr("%n restricted zone(s) nearby", nullptr, static_cast<int>(nearby.count())));
    html += row(tr("Zones"), escaped(summary.join(QStringLiteral(", ")) + QChar(u'.')));
    if (briefing.approximations) {
        html += row(tr("Approximate"), escaped(tr("Some results are approximate, and conservative where data is missing: %1.")
                                                   .arg(GeoZonePathEvaluator::approximationTexts(briefing.approximations).join(QStringLiteral("; ")))));
    }
    html += QStringLiteral("</table>\n");

    const auto section = [&html, &briefing](const QString &heading, const QString &empty, const QList<const GeoZoneBriefing::Zone*> &zones) {
        html += QStringLiteral("<h2>%1</h2>\n").arg(heading.toHtmlEscaped());
        if (zones.isEmpty()) {
            html += QStringLiteral("<p>%1</p>\n").arg(empty.toHtmlEscaped());
        }
        for (const GeoZoneBriefing::Zone *zone : zones) {
            html += zoneHtml(*zone, briefing);
        }
    };
    section(tr("Zones the flight path enters (%1)").arg(entered.count()), tr("None."), entered);
    section(tr("Zones only returns would enter (%1)").arg(contingency.count()), tr("None."), contingency);
    section(tr("Restricted zones within %1 (%2)").arg(distanceText(briefing.nearbyDistance)).arg(nearby.count()), tr("None."), nearby);

    html += QStringLiteral("<h2>%1</h2>\n").arg(tr("Data versions").toHtmlEscaped());
    if (briefing.sources.isEmpty()) {
        html += QStringLiteral("<p>%1</p>\n").arg(tr("No geozone data covers the mission.").toHtmlEscaped());
    }
    for (const GeoZoneSourceVersion &source : briefing.sources) {
        html += QStringLiteral("<table class=\"zone\" cellspacing=\"0\" cellpadding=\"4\" width=\"100%\">\n");
        html += QStringLiteral("<tr><td colspan=\"2\" class=\"zonetitle\" bgcolor=\"#455a64\"><font color=\"#ffffff\"><b>%1</b></font></td></tr>\n")
                    .arg((source.name.isEmpty() ? source.key : source.name).toHtmlEscaped());
        QStringList format = { source.format, source.provider };
        format.removeAll(QString());
        html += row(tr("Format"), escaped(format.join(QStringLiteral(", "))));
        html += row(tr("File or feed"), escaped(source.key));
        html += row(tr("Data date"), escaped(dateTimeText(source.dataDate)));
        if (!source.airacCycle.isEmpty()) {
            html += row(tr("AIRAC cycle"), escaped(tr("%1, at the data date").arg(source.airacCycle)));
        }
        if (source.validFrom.isValid() || source.validTo.isValid()) {
            html += row(tr("Valid"), escaped(tr("%1 to %2").arg(source.validFrom.isValid() ? dateTimeText(source.validFrom) : tr("open"),
                                                              source.validTo.isValid() ? dateTimeText(source.validTo) : tr("open"))));
        }
        html += row(tr("Checked"), escaped(dateTimeText(source.checkedAt)));
        html += row(tr("Content hash"), QStringLiteral("<code>%1</code>").arg(source.contentHash.isEmpty() ? tr("none") : QString::fromLatin1(source.contentHash.toHex())));
        html += QStringLiteral("</table>\n");
    }
    html += QStringLiteral("</body></html>\n");
    return html;
}

bool GeoZoneBriefingExport::writePdf(const GeoZoneBriefing &briefing, const QString &fileName, QString &errorString)
{
    const QString title = tr("Geozone briefing: %1").arg(briefing.mission.name);
    QPdfWriter writer(fileName);
    writer.setTitle(title);
    writer.setCreator(QStringLiteral("QGroundControl"));
    writer.setResolution(300);
    writer.setPageLayout(QPageLayout(QPageSize(QPageSize::A4), QPageLayout::Portrait, QMarginsF(15, 15, 15, 15), QPageLayout::Millimeter));

    QPainter painter;
    if (!painter.begin(&writer)) {
        errorString = tr("Can't write %1").arg(fileName);
        return false;
    }

    QTextDocument document;
    document.documentLayout()->setPaintDevice(&writer);
    QFont font = document.defaultFont();
    font.setPointSizeF(9);
    document.setDefaultFont(font);
    document.setHtml(toHtml(briefing, true));

    QFont marginFont = font;
    marginFont.setPointSizeF(7.5);
    const QFontMetricsF metrics(marginFont, &writer);
    const QRectF page(QPointF(0, 0), QSizeF(writer.width(), writer.height()));
    const double header = metrics.height() * 2;
    const double footer = metrics.height() * 2;
    const QSizeF body(page.width(), page.height() - header - footer);
    document.setPageSize(body);
    const int pageCount = document.pageCount();
    const QString date = dateTimeText(briefing.generatedAt);

    for (int i = 0; i < pageCount; i++) {
        if ((i > 0) && !writer.newPage()) {
            errorString = tr("Can't write %1").arg(fileName);
            return false;
        }
        painter.save();
        painter.setFont(marginFont);
        painter.setPen(QColor(0x55, 0x55, 0x55));
        const QRectF headerRect(0, 0, page.width(), metrics.height());
        painter.drawText(headerRect, Qt::AlignLeft | Qt::AlignVCenter, metrics.elidedText(title, Qt::ElideRight, page.width() * 0.65));
        painter.drawText(headerRect, Qt::AlignRight | Qt::AlignVCenter, date);
        painter.drawLine(QPointF(0, metrics.height() * 1.3), QPointF(page.width(), metrics.height() * 1.3));
        painter.drawText(QRectF(0, page.height() - metrics.height(), page.width(), metrics.height()), Qt::AlignHCenter | Qt::AlignVCenter,
                         tr("Page %1 of %2").arg(i + 1).arg(pageCount));
        painter.restore();

        painter.save();
        painter.translate(0, header - (i * body.height()));
        QAbstractTextDocumentLayout::PaintContext context;
        context.clip = QRectF(0, i * body.height(), body.width(), body.height());
        painter.setClipRect(context.clip);
        document.documentLayout()->draw(&painter, context);
        painter.restore();
    }
    if (!painter.end()) {
        errorString = tr("Can't write %1").arg(fileName);
        return false;
    }
    if (QFileInfo(fileName).size() <= 0) {
        errorString = tr("Can't write %1").arg(fileName);
        return false;
    }
    return true;
}

bool GeoZoneBriefingExport::write(const GeoZoneBriefing &briefing, const QString &fileName, Format format, QString &errorString)
{
    if (format == Format::Pdf) {
        return writePdf(briefing, fileName, errorString);
    }
    const QByteArray content = (format == Format::Json) ? QJsonDocument(toJson(briefing)).toJson(QJsonDocument::Indented) : toHtml(briefing).toUtf8();
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly) || (file.write(content) != content.size()) || !file.commit()) {
        errorString = tr("Can't write %1: %2").arg(fileName, file.errorString());
        return false;
    }
    return true;
}
