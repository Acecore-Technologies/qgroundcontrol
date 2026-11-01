/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneAlertLog.h"
#include "AppSettings.h"
#include "GeoZoneSettings.h"
#include "SettingsManager.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaEnum>
#include <QtCore/QRegularExpression>

namespace {

QString csvField(const QString &text)
{
    // Text from feeds starting like a formula is run by spreadsheets, a leading quote keeps it text
    static const QString formulaStarts = QStringLiteral("=+-@\t\r");
    const QString value = (!text.isEmpty() && formulaStarts.contains(text.front())) ? (QLatin1Char('\'') + text) : text;
    if (!value.contains(QLatin1Char(',')) && !value.contains(QLatin1Char('"')) && !value.contains(QLatin1Char('\n')) && !value.contains(QLatin1Char('\r'))) {
        return value;
    }
    QString quoted = value;
    (void) quoted.replace(QStringLiteral("\""), QStringLiteral("\"\""));
    return QStringLiteral("\"%1\"").arg(quoted);
}

QString number(double value, int decimals)
{
    return qIsNaN(value) ? QString() : QString::number(value, 'f', decimals);
}

QString eventText(GeoZoneMonitor::AlertEvent::Kind kind)
{
    switch (kind) {
    case GeoZoneMonitor::AlertEvent::Entry:
        return QStringLiteral("entered");
    case GeoZoneMonitor::AlertEvent::PositionLost:
        return QStringLiteral("position_lost");
    case GeoZoneMonitor::AlertEvent::Traffic:
        return QStringLiteral("traffic_entered");
    case GeoZoneMonitor::AlertEvent::Approach:
        break;
    }
    return QStringLiteral("approaching");
}

} // namespace

GeoZoneAlertLog::GeoZoneAlertLog(QObject *parent)
    : QObject(parent)
{
    _flushTimer.setSingleShot(true);
    _flushTimer.setInterval(flushIntervalMSecs);
    (void) connect(&_flushTimer, &QTimer::timeout, this, &GeoZoneAlertLog::flush);
}

GeoZoneAlertLog::~GeoZoneAlertLog()
{
    flush();
}

void GeoZoneAlertLog::setDirectory(const QString &directory)
{
    flush();
    _file.close();
    _directory = directory;
}

QString GeoZoneAlertLog::filePath(const QDate &date) const
{
    const QString directory = _directory.isEmpty() ? SettingsManager::instance()->appSettings()->telemetrySavePath() : _directory;
    if (directory.isEmpty()) {
        return QString();
    }
    return QDir(directory).filePath(QStringLiteral("GeoZoneAlerts-%1.csv").arg(date.toString(Qt::ISODate)));
}

QString GeoZoneAlertLog::header()
{
    return QStringLiteral("time_utc,vehicle,latitude,longitude,altitude_amsl_m,terrain_amsl_m,event,distance_m,zone_identifier,zone_name,restriction,source,time_to_entry_s,traffic");
}

QString GeoZoneAlertLog::line(const GeoZoneMonitor::AlertEvent &event)
{
    const QMetaEnum restrictions = QMetaEnum::fromType<GeoZone::Restriction>();
    return QStringList{
        event.time.toUTC().toString(Qt::ISODateWithMs),
        (event.vehicleId == GeoZoneMonitor::noVehicle) ? QString() : QString::number(event.vehicleId),
        number(event.position.coordinate.latitude(), 7),
        number(event.position.coordinate.longitude(), 7),
        number(event.position.altitudeAMSL, 1),
        number(event.position.terrainAMSL, 1),
        eventText(event.kind),
        number(event.distance, 0),
        csvField(event.zoneIdentifier),
        csvField(event.zoneName),
        QString::fromLatin1(restrictions.valueToKey(event.restriction)),
        csvField(event.sourceText),
        number(event.timeToEntry, 0),
        csvField(event.traffic),
    }.join(QLatin1Char(','));
}

void GeoZoneAlertLog::append(const GeoZoneMonitor::AlertEvent &event)
{
    if (!GeoZoneSettings::instance()->logAlerts()->rawValue().toBool()) {
        return;
    }

    const QDate date = event.time.toUTC().date();
    const QString path = filePath(date);
    if (path.isEmpty()) {
        return;
    }
    if (path != _bufferPath) {
        flush();
        _bufferPath = path;
        _bufferDate = date;
    }
    _buffer += (line(event) + QLatin1Char('\n')).toUtf8();
    if (!_flushTimer.isActive()) {
        _flushTimer.start();
    }
}

void GeoZoneAlertLog::flush()
{
    _flushTimer.stop();
    if (_buffer.isEmpty()) {
        return;
    }
    if (((_file.fileName() != _bufferPath) || !_file.isOpen()) && !_open()) {
        _buffer.clear();
        return;
    }
    if ((_file.write(_buffer) != _buffer.size()) || !_file.flush()) {
        qCWarning(GeoZoneMonitorLog) << "Unable to write geozone alert log" << _file.fileName() << _file.errorString();
        _file.close();
    }
    _buffer.clear();
}

bool GeoZoneAlertLog::_open()
{
    _file.close();
    (void) QDir().mkpath(QFileInfo(_bufferPath).absolutePath());
    _file.setFileName(_bufferPath);
    if (!_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        qCWarning(GeoZoneMonitorLog) << "Unable to write geozone alert log" << _bufferPath << _file.errorString();
        return false;
    }
    if (_file.size() == 0) {
        (void) _file.write((header() + QLatin1Char('\n')).toUtf8());
    }

    // A file is started each day, a good time to delete those past the retention
    const int retentionDays = GeoZoneSettings::instance()->alertLogRetentionDays()->rawValue().toInt();
    if (retentionDays > 0) {
        (void) removeOldFiles(_bufferDate, retentionDays);
    }
    return true;
}

int GeoZoneAlertLog::removeOldFiles(const QDate &today, int days)
{
    const QString path = filePath(today);
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
