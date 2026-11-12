/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneAlertHistory.h"
#include "AppSettings.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneInfo.h"
#include "GeoZoneManager.h"
#include "GeoZoneSettings.h"
#include "GeoZoneText.h"
#include "SettingsManager.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaEnum>
#include <QtCore/QRegularExpression>
#include <QtCore/QTextStream>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

/// Text from feeds starting like a formula is run by spreadsheets, the log keeps it text with a leading quote
constexpr QLatin1StringView formulaStarts("=+-@\t\r");

/// Names of the columns of the log by GeoZoneAlertHistory::LogColumn, which the header, the writer and the reader share
constexpr std::array<const char*, GeoZoneAlertHistory::LogColumnCount> columnNames = {
    "time_utc", "vehicle", "latitude", "longitude", "altitude_amsl_m", "terrain_amsl_m", "event", "distance_m", "zone_identifier", "zone_name", "restriction",
    "source", "time_to_entry_s", "traffic", "alert_id", "detail", "warning_distance_m", "warning_vertical_m", "warning_time_s", "ground_speed_mps",
    "vertical_speed_mps", "level",
};

// Decimals of the numbers of the log
constexpr int coordinateDecimals = 7;
constexpr int altitudeDecimals = 1;
constexpr int speedDecimals = 1;
constexpr int wholeNumber = 0;

/// Event column of the log for each GeoZoneMonitor::AlertEvent::Kind
struct EventName {
    GeoZoneMonitor::AlertEvent::Kind kind;
    const char *name;
};

constexpr EventName eventNames[] = {
    { GeoZoneMonitor::AlertEvent::Approach, "approaching" },
    { GeoZoneMonitor::AlertEvent::Entry, "entered" },
    { GeoZoneMonitor::AlertEvent::PositionLost, "position_lost" },
    { GeoZoneMonitor::AlertEvent::Traffic, "traffic_entered" },
    { GeoZoneMonitor::AlertEvent::NonConformance, "outside_volume" },
    { GeoZoneMonitor::AlertEvent::OnGround, "on_ground" },
    { GeoZoneMonitor::AlertEvent::Awareness, "geo_awareness" },
    { GeoZoneMonitor::AlertEvent::FenceWarning, "fence_warning" },
    { GeoZoneMonitor::AlertEvent::FenceBreach, "fence_breached" },
    { GeoZoneMonitor::AlertEvent::Activated, "activated" },
    { GeoZoneMonitor::AlertEvent::ActivationSoon, "activation_soon" },
    { GeoZoneMonitor::AlertEvent::Notified, "notified" },
    { GeoZoneMonitor::AlertEvent::Approved, "approved" },
    { GeoZoneMonitor::AlertEvent::Dismissed, "dismissed" },
    { GeoZoneMonitor::AlertEvent::Muted, "muted" },
    { GeoZoneMonitor::AlertEvent::Permit, "permit" },
    { GeoZoneMonitor::AlertEvent::PermitEnded, "permit_ended" },
    { GeoZoneMonitor::AlertEvent::SourceOff, "source_off" },
    { GeoZoneMonitor::AlertEvent::SourceOn, "source_on" },
    { GeoZoneMonitor::AlertEvent::Authorised, "authorisation" },
    { GeoZoneMonitor::AlertEvent::AuthorisationRemoved, "authorisation_removed" },
};

QString eventName(GeoZoneMonitor::AlertEvent::Kind kind)
{
    for (const EventName &event : eventNames) {
        if (event.kind == kind) {
            return QString::fromLatin1(event.name);
        }
    }
    return QString();
}

std::optional<GeoZoneMonitor::AlertEvent::Kind> eventKind(const QString &name)
{
    for (const EventName &event : eventNames) {
        if (name == QLatin1String(event.name)) {
            return event.kind;
        }
    }
    return std::nullopt;
}

/// A record of the log is one line, which its readers rely on: line breaks in text from feeds, like a zone name, are
/// written as a backslash and n, and backslashes doubled
QString escapedLineBreaks(const QString &text)
{
    if (!text.contains(QLatin1Char('\n')) && !text.contains(QLatin1Char('\r')) && !text.contains(QLatin1Char('\\'))) {
        return text;
    }
    QString escaped;
    escaped.reserve(text.size() + 8);
    for (qsizetype i = 0; i < text.size(); i++) {
        const QChar character = text.at(i);
        if (character == QLatin1Char('\\')) {
            escaped += QStringLiteral("\\\\");
        } else if (character == QLatin1Char('\r')) {
            // With the line feed which follows it, one line break
            if ((i + 1 < text.size()) && (text.at(i + 1) == QLatin1Char('\n'))) {
                i++;
            }
            escaped += QStringLiteral("\\n");
        } else if (character == QLatin1Char('\n')) {
            escaped += QStringLiteral("\\n");
        } else {
            escaped += character;
        }
    }
    return escaped;
}

/// Undoes escapedLineBreaks()
QString unescapedLineBreaks(const QString &text)
{
    if (!text.contains(QLatin1Char('\\'))) {
        return text;
    }
    QString unescaped;
    unescaped.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); i++) {
        const QChar character = text.at(i);
        const QChar next = (i + 1 < text.size()) ? text.at(i + 1) : QChar();
        if ((character == QLatin1Char('\\')) && ((next == QLatin1Char('\\')) || (next == QLatin1Char('n')))) {
            unescaped += (next == QLatin1Char('n')) ? QLatin1Char('\n') : QLatin1Char('\\');
            i++;
        } else {
            unescaped += character;
        }
    }
    return unescaped;
}

QString csvField(const QString &text)
{
    const QString escaped = escapedLineBreaks(text);
    const QString value = (!escaped.isEmpty() && formulaStarts.contains(escaped.front())) ? (QLatin1Char('\'') + escaped) : escaped;
    if (!value.contains(QLatin1Char(',')) && !value.contains(QLatin1Char('"'))) {
        return value;
    }
    QString quoted = value;
    (void) quoted.replace(QStringLiteral("\""), QStringLiteral("\"\""));
    return QStringLiteral("\"%1\"").arg(quoted);
}

/// Splits a CSV line of the log, undoing its quoting
QStringList csvFields(const QString &line)
{
    QStringList fields;
    QString field;
    bool quoted = false;
    for (qsizetype i = 0; i < line.size(); i++) {
        const QChar character = line.at(i);
        if (quoted) {
            if (character == QLatin1Char('"')) {
                if ((i + 1 < line.size()) && (line.at(i + 1) == QLatin1Char('"'))) {
                    field += character;
                    i++;
                } else {
                    quoted = false;
                }
            } else {
                field += character;
            }
        } else if (character == QLatin1Char('"')) {
            quoted = true;
        } else if (character == QLatin1Char(',')) {
            fields.append(field);
            field.clear();
        } else {
            field += character;
        }
    }
    fields.append(field);

    for (QString &text : fields) {
        if ((text.size() >= 2) && (text.front() == QLatin1Char('\'')) && formulaStarts.contains(text.at(1))) {
            text.remove(0, 1);
        }
        text = unescapedLineBreaks(text);
    }
    return fields;
}

QString numberText(double value, int decimals)
{
    return qIsNaN(value) ? QString() : QString::number(value, 'f', decimals);
}

double numberValue(const QString &text)
{
    bool ok = false;
    const double value = text.toDouble(&ok);
    return ok ? value : qQNaN();
}

} // namespace

GeoZoneAlertHistory::GeoZoneAlertHistory(GeoZoneManager *manager, QObject *parent)
    : QAbstractListModel(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
{
    _shownTimer.setSingleShot(true);
    _shownTimer.setInterval(shownPositionMSecs);
    (void) connect(&_shownTimer, &QTimer::timeout, this, &GeoZoneAlertHistory::clearShownPosition);
    _logFlushTimer.setSingleShot(true);
    _logFlushTimer.setInterval(logFlushIntervalMSecs);
    (void) connect(&_logFlushTimer, &QTimer::timeout, this, &GeoZoneAlertHistory::flushLog);
}

GeoZoneAlertHistory::~GeoZoneAlertHistory()
{
    // At exit the settings of the application may be destroyed before the monitor which owns this: the lines not
    // written yet go to the folder read last, and the old logs are left to the next start
    _closing = true;
    if (!flushLog()) {
        qCWarning(GeoZoneMonitorLog) << "Geozone alert log lines not written:" << _logLines.count() << _logError;
    }
}

int GeoZoneAlertHistory::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant GeoZoneAlertHistory::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || (index.row() >= _entries.count())) {
        return QVariant();
    }
    const Entry &entry = _entries[index.row()];
    switch (role) {
    case TimeRole:
        return entry.event.time.toLocalTime();
    case MessageRole:
        return entry.event.message;
    case ZoneNameRole:
        return entry.event.zoneName.isEmpty() ? entry.event.zoneIdentifier : entry.event.zoneName;
    case RestrictionRole:
        return static_cast<int>(entry.event.restriction);
    case InsideRole:
        return entry.event.inside;
    case CoordinateRole:
        return QVariant::fromValue(entry.event.position.coordinate);
    case AltitudeRole:
        return entry.event.position.altitudeAMSL;
    case AcknowledgedRole:
        return entry.acknowledged;
    case EarlierRole:
        return entry.earlier;
    case LevelRole:
        return static_cast<int>(entry.event.level);
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> GeoZoneAlertHistory::roleNames() const
{
    // Built once, the views ask for them again and again
    static const QHash<int, QByteArray> names = {
        { TimeRole, "time" },
        { MessageRole, "message" },
        { ZoneNameRole, "zoneName" },
        { RestrictionRole, "restriction" },
        { InsideRole, "inside" },
        { CoordinateRole, "coordinate" },
        { AltitudeRole, "altitudeAMSL" },
        { AcknowledgedRole, "acknowledged" },
        { EarlierRole, "earlier" },
        { LevelRole, "level" },
    };
    return names;
}

int GeoZoneAlertHistory::unacknowledgedCount() const
{
    return static_cast<int>(std::count_if(_entries.cbegin(), _entries.cend(), [](const Entry &entry) { return !entry.acknowledged; }));
}

void GeoZoneAlertHistory::append(const GeoZoneMonitor::AlertEvent &event)
{
    if (_entries.count() >= maxEntries) {
        beginRemoveRows(QModelIndex(), maxEntries - 1, static_cast<int>(_entries.count()) - 1);
        _entries.resize(maxEntries - 1);
        endRemoveRows();
    }
    beginInsertRows(QModelIndex(), 0, 0);
    _entries.prepend(Entry{ event, false, false });
    endInsertRows();
    emit countChanged();
    emit unacknowledgedCountChanged();
    _log(event);
}

void GeoZoneAlertHistory::logPilotEvent(const GeoZoneMonitor::AlertEvent &event)
{
    _log(event);
}

void GeoZoneAlertHistory::_log(const GeoZoneMonitor::AlertEvent &event)
{
    if (!_logging) {
        return;
    }
    // Kept until written. When the log can't be written for long, the oldest lines make room and are counted as lost.
    const QString previousStatus = logStatus();
    while (_logLines.count() >= maxPendingLogLines) {
        _logLines.removeFirst();
        _lostLogLines++;
        qCWarning(GeoZoneMonitorLog) << "Geozone alert log lines lost:" << _lostLogLines;
    }
    _logLines.append(LogLine{ event.time.toUTC().date(), (logLine(event) + QLatin1Char('\n')).toUtf8() });
    if (logStatus() != previousStatus) {
        emit logHealthChanged();
    }
    if (!_logFlushTimer.isActive()) {
        _logFlushTimer.start(logFlushIntervalMSecs);
    }
}

QString GeoZoneAlertHistory::logStatus() const
{
    QStringList parts;
    if (logFailing()) {
        parts.append(tr("The alert log can't be written: %1.").arg(_logError));
        if (!_logLines.isEmpty()) {
            parts.append(tr("%n alert log line(s) are kept and written when it can be again.", nullptr, static_cast<int>(_logLines.count())));
        }
    }
    if (_lostLogLines > 0) {
        parts.append(tr("%n alert log line(s) were lost.", nullptr, _lostLogLines));
    }
    return parts.join(QLatin1Char(' '));
}


void GeoZoneAlertHistory::startLog(const QString &directory)
{
    (void) flushLog();
    _logFile.close();
    _logDirectory = directory;
    _logging = true;
    _loadLog(logFilePath(GeoZoneEnvironment::currentDateTimeUtc().date()));
    // Lines the previous folder couldn't take go to this one
    (void) flushLog();
}

QString GeoZoneAlertHistory::logFilePath(const QDate &date) const
{
    if (_logDirectory.isEmpty() && !_closing) {
        _settingsDirectory = SettingsManager::instance()->appSettings()->telemetrySavePath();
    }
    const QString directory = _logDirectory.isEmpty() ? _settingsDirectory : _logDirectory;
    if (directory.isEmpty()) {
        return QString();
    }
    return QDir(directory).filePath(QStringLiteral("GeoZoneAlerts-%1.csv").arg(date.toString(Qt::ISODate)));
}

QString GeoZoneAlertHistory::logColumnName(LogColumn column)
{
    return ((column >= 0) && (column < LogColumnCount)) ? QString::fromLatin1(columnNames[column]) : QString();
}

QString GeoZoneAlertHistory::logHeader()
{
    QStringList names;
    for (const char *name : columnNames) {
        names.append(QString::fromLatin1(name));
    }
    return names.join(QLatin1Char(','));
}

QString GeoZoneAlertHistory::logLine(const GeoZoneMonitor::AlertEvent &event)
{
    const QMetaEnum restrictions = QMetaEnum::fromType<GeoZone::Restriction>();
    QStringList fields(LogColumnCount);
    fields[TimeColumn] = event.time.toUTC().toString(Qt::ISODateWithMs);
    fields[VehicleColumn] = (event.vehicleId == GeoZoneMonitor::noVehicle) ? QString() : QString::number(event.vehicleId);
    fields[LatitudeColumn] = numberText(event.position.coordinate.latitude(), coordinateDecimals);
    fields[LongitudeColumn] = numberText(event.position.coordinate.longitude(), coordinateDecimals);
    fields[AltitudeColumn] = numberText(event.position.altitudeAMSL, altitudeDecimals);
    fields[TerrainColumn] = numberText(event.position.terrainAMSL, altitudeDecimals);
    fields[EventColumn] = eventName(event.kind);
    fields[DistanceColumn] = numberText(event.distance, wholeNumber);
    fields[ZoneIdentifierColumn] = csvField(event.zoneIdentifier);
    fields[ZoneNameColumn] = csvField(event.zoneName);
    fields[RestrictionColumn] = QString::fromLatin1(restrictions.valueToKey(event.restriction));
    fields[SourceColumn] = csvField(event.sourceText);
    fields[TimeToEntryColumn] = numberText(event.timeToEntry, wholeNumber);
    fields[TrafficColumn] = csvField(event.traffic);
    fields[AlertIdColumn] = (event.id > 0) ? QString::number(event.id) : QString();
    // The circumstance of an alert about a zone, or the text of the others
    fields[DetailColumn] = csvField((event.circumstance != GeoZoneMonitor::AlertEvent::Circumstance::None) ?
                                        GeoZoneMonitor::AlertEvent::circumstanceCode(event.circumstance) : event.detail);
    fields[WarningDistanceColumn] = numberText(event.warningDistance, wholeNumber);
    fields[WarningVerticalColumn] = numberText(event.warningVerticalDistance, wholeNumber);
    fields[WarningTimeColumn] = numberText(event.warningTime, wholeNumber);
    fields[GroundSpeedColumn] = numberText(event.groundSpeed, speedDecimals);
    fields[VerticalSpeedColumn] = numberText(event.verticalSpeed, speedDecimals);
    fields[LevelColumn] = GeoZoneAlertLevel::code(event.level);
    return fields.join(QLatin1Char(','));
}

bool GeoZoneAlertHistory::flushLog()
{
    _logFlushTimer.stop();
    if (_logLines.isEmpty()) {
        return true;
    }
    const QString previousStatus = logStatus();
    const QString error = _writeLog();
    if (!error.isEmpty()) {
        if (error != _logError) {
            qCWarning(GeoZoneMonitorLog) << "Unable to write geozone alert log, lines kept:" << _logLines.count() << error;
        }
        // Tried again, the folder may come back or get room
        _logFlushTimer.start(logRetryIntervalMSecs);
    }
    _logError = error;
    if (logStatus() != previousStatus) {
        emit logHealthChanged();
    }
    return _logLines.isEmpty();
}

QString GeoZoneAlertHistory::_writeLog()
{
    while (!_logLines.isEmpty()) {
        const QDate date = _logLines.constFirst().date;
        const QString path = logFilePath(date);
        if (path.isEmpty()) {
            return tr("no telemetry folder is set");
        }
        if ((_logFile.fileName() != path) || !_logFile.isOpen()) {
            const QString error = _openLog(path, date);
            if (!error.isEmpty()) {
                return error;
            }
        }
        // The lines of the day, each written whole or tried again
        while (!_logLines.isEmpty() && (_logLines.constFirst().date == date)) {
            const QByteArray &text = _logLines.constFirst().text;
            const qint64 written = _logFile.write(text);
            if (written != text.size()) {
                const QString error = _logFile.errorString();
                _logLineTorn = _logLineTorn || (written > 0);
                _logFile.close();
                return error;
            }
            _logLines.removeFirst();
        }
    }
    return QString();
}

QString GeoZoneAlertHistory::_openLog(const QString &path, const QDate &date)
{
    _logFile.close();
    (void) QDir().mkpath(QFileInfo(path).absolutePath());
    _logFile.setFileName(path);
    // Unbuffered, so that a write tells whether the line reached the file
    if (!_logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text | QIODevice::Unbuffered)) {
        return _logFile.errorString();
    }
    const QByteArray start = (_logFile.size() == 0) ? (logHeader() + QLatin1Char('\n')).toUtf8() : (_logLineTorn ? QByteArrayLiteral("\n") : QByteArray());
    if (!start.isEmpty() && (_logFile.write(start) != start.size())) {
        const QString error = _logFile.errorString();
        _logFile.close();
        return error;
    }
    _logLineTorn = false;

    // A file is started each day, a good time to delete those past the retention
    if (!_closing) {
        (void) removeOldLogs(date, GeoZoneSettings::instance()->recordRetention()->rawValue().toInt());
    }
    return QString();
}

int GeoZoneAlertHistory::removeOldLogs(const QDate &today, int days)
{
    const QString path = logFilePath(today);
    if (path.isEmpty() || (days <= 0)) {
        return 0;
    }
    const QString directory = QFileInfo(path).absolutePath();
    static const QRegularExpression namePattern(QStringLiteral("^GeoZoneAlerts-(\\d{4}-\\d{2}-\\d{2})\\.csv$"));
    const QDate oldest = today.addDays(1 - days);
    int removed = 0;
    QDir dir(directory);
    for (const QString &name : dir.entryList({ QStringLiteral("GeoZoneAlerts-*.csv") }, QDir::Files)) {
        const QRegularExpressionMatch match = namePattern.match(name);
        const QDate date = match.hasMatch() ? QDate::fromString(match.captured(1), Qt::ISODate) : QDate();
        if (date.isValid() && (date < oldest) && dir.remove(name)) {
            removed++;
        }
    }
    if (removed > 0) {
        qCDebug(GeoZoneMonitorLog) << "Removed" << removed << "geozone alert logs before" << oldest;
    }
    return removed;
}

void GeoZoneAlertHistory::_loadLog(const QString &filePath)
{
    QFile file(filePath);
    if (filePath.isEmpty() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    // Appended after the alerts of this session, the newest first
    QList<Entry> earlier;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const std::optional<GeoZoneMonitor::AlertEvent> event = parseLogLine(stream.readLine());
        if (event && !event->pilotEvent()) {
            earlier.prepend(Entry{ *event, true, true });
        }
    }
    const qsizetype room = maxEntries - _entries.count();
    if (earlier.count() > room) {
        earlier.resize(room);
    }
    if (earlier.isEmpty()) {
        return;
    }
    beginInsertRows(QModelIndex(), static_cast<int>(_entries.count()), static_cast<int>(_entries.count() + earlier.count()) - 1);
    _entries.append(earlier);
    endInsertRows();
    emit countChanged();
}

QStringList GeoZoneAlertHistory::logFields(const QString &line)
{
    return csvFields(line);
}

std::optional<GeoZoneMonitor::AlertEvent> GeoZoneAlertHistory::parseLogLine(const QString &line)
{
    // Logs written before the last columns were added lack them
    const QStringList fields = csvFields(line);
    const auto text = [&fields](LogColumn column) { return fields.value(column); };
    const auto number = [&fields](LogColumn column) { return (fields.count() > column) ? numberValue(fields[column]) : qQNaN(); };
    if ((fields.count() < minLogColumns) || (text(TimeColumn) == logColumnName(TimeColumn))) {
        return std::nullopt;
    }
    GeoZoneMonitor::AlertEvent event;
    event.time = QDateTime::fromString(text(TimeColumn), Qt::ISODateWithMs);
    const std::optional<GeoZoneMonitor::AlertEvent::Kind> kind = eventKind(text(EventColumn));
    if (!kind || !event.time.isValid()) {
        return std::nullopt;
    }
    event.kind = *kind;
    bool vehicleOk = false;
    const int vehicleId = text(VehicleColumn).toInt(&vehicleOk);
    event.vehicleId = vehicleOk ? vehicleId : GeoZoneMonitor::noVehicle;
    event.position.coordinate = QGeoCoordinate(number(LatitudeColumn), number(LongitudeColumn));
    event.position.altitudeAMSL = number(AltitudeColumn);
    event.position.terrainAMSL = number(TerrainColumn);
    event.distance = number(DistanceColumn);
    event.inside = (event.kind == GeoZoneMonitor::AlertEvent::Entry) || (event.kind == GeoZoneMonitor::AlertEvent::Traffic) ||
                   (event.kind == GeoZoneMonitor::AlertEvent::OnGround) ||
                   (event.kind == GeoZoneMonitor::AlertEvent::Activated) ||
                   ((event.kind == GeoZoneMonitor::AlertEvent::ActivationSoon) && (event.distance == 0)) ||
                   ((event.kind == GeoZoneMonitor::AlertEvent::PositionLost) && (event.distance == 0));
    event.zoneIdentifier = text(ZoneIdentifierColumn);
    event.zoneName = text(ZoneNameColumn);
    bool restrictionOk = false;
    const int restriction = QMetaEnum::fromType<GeoZone::Restriction>().keyToValue(text(RestrictionColumn).toLatin1().constData(), &restrictionOk);
    event.restriction = restrictionOk ? static_cast<GeoZone::Restriction>(restriction) : GeoZone::Unknown;
    event.sourceText = text(SourceColumn);
    event.timeToEntry = number(TimeToEntryColumn);
    event.traffic = text(TrafficColumn);
    event.detail = text(DetailColumn);
    event.circumstance = GeoZoneMonitor::AlertEvent::circumstanceFromCode(event.kind, event.detail);
    if (event.circumstance != GeoZoneMonitor::AlertEvent::Circumstance::None) {
        event.detail.clear();
    }
    event.warningDistance = number(WarningDistanceColumn);
    event.warningVerticalDistance = number(WarningVerticalColumn);
    event.warningTime = number(WarningTimeColumn);
    event.groundSpeed = number(GroundSpeedColumn);
    event.verticalSpeed = number(VerticalSpeedColumn);
    // Logs written before the levels have the level of the alert
    bool levelOk = false;
    event.level = GeoZoneAlertLevel::fromCode(text(LevelColumn), &levelOk);
    if (!levelOk) {
        event.level = GeoZoneMonitor::alertLevel(event);
    }

    // The messages of the monitor
    const QString name = event.zoneName.isEmpty() ? event.zoneIdentifier : event.zoneName;
    event.message = GeoZoneMonitor::alertMessage(event, name, GeoZoneText::restriction(event.restriction));
    return event;
}

void GeoZoneAlertHistory::acknowledge(int row)
{
    _acknowledge(row, QStringLiteral("Acknowledge"));
}

void GeoZoneAlertHistory::_acknowledge(int row, const QString &pilotAction)
{
    if ((row < 0) || (row >= _entries.count()) || _entries[row].acknowledged) {
        return;
    }
    if (!pilotAction.isEmpty()) {
        emit pilotAcknowledged(_entries[row].event, pilotAction);
    }
    _entries[row].acknowledged = true;
    emit dataChanged(index(row), index(row), { AcknowledgedRole });
    emit unacknowledgedCountChanged();
    if (_entries[row].event.id > 0) {
        emit alertAcknowledged(_entries[row].event.id);
    }
}

void GeoZoneAlertHistory::acknowledgeAlert(int alertId)
{
    if (alertId <= 0) {
        return;
    }
    const auto it = std::find_if(_entries.cbegin(), _entries.cend(), [alertId](const Entry &entry) { return entry.event.id == alertId; });
    if (it != _entries.cend()) {
        // The monitor logs what the pilot answered in the popup
        _acknowledge(static_cast<int>(std::distance(_entries.cbegin(), it)), QString());
    }
}

void GeoZoneAlertHistory::acknowledgeAll()
{
    if (unacknowledgedCount() == 0) {
        return;
    }
    // The oldest first, like they were raised
    for (auto entry = _entries.rbegin(); entry != _entries.rend(); ++entry) {
        if (!entry->acknowledged) {
            emit pilotAcknowledged(entry->event, QStringLiteral("Acknowledge all"));
            entry->acknowledged = true;
        }
    }
    emit dataChanged(index(0), index(count() - 1), { AcknowledgedRole });
    emit unacknowledgedCountChanged();
    emit allAcknowledged();
}

void GeoZoneAlertHistory::show(int row)
{
    if ((row < 0) || (row >= _entries.count())) {
        return;
    }
    _acknowledge(row, QStringLiteral("Show on map"));
    const GeoZoneMonitor::AlertEvent &event = _entries[row].event;
    _shownPosition = event.position.coordinate;
    _shownTimer.start();
    emit shownPositionChanged();

    // The zone of a session alert by its key, otherwise one with its identifier and name
    const QList<GeoZone*> &zones = _manager->zoneList();
    auto zone = std::find_if(zones.cbegin(), zones.cend(), [&event](const GeoZone *candidate) { return !event.zoneKey.isEmpty() && (candidate->key() == event.zoneKey); });
    if (zone == zones.cend()) {
        zone = std::find_if(zones.cbegin(), zones.cend(), [&event](const GeoZone *candidate) {
            return (candidate->identifier() == event.zoneIdentifier) && (candidate->name() == event.zoneName);
        });
    }
    if (zone != zones.cend()) {
        _manager->showZone(QVariant::fromValue(GeoZoneInfo(*zone)));
    }
}

void GeoZoneAlertHistory::clearShownPosition()
{
    _shownTimer.stop();
    if (_shownPosition.isValid()) {
        _shownPosition = QGeoCoordinate();
        emit shownPositionChanged();
    }
}
