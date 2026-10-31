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
#include <QtCore/QFile>
#include <QtCore/QMetaEnum>

namespace {

QString csvField(const QString &value)
{
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

} // namespace

GeoZoneAlertLog::GeoZoneAlertLog(QObject *parent)
    : QObject(parent)
{
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
    return QStringLiteral("time_utc,vehicle,latitude,longitude,altitude_amsl_m,terrain_amsl_m,event,distance_m,zone_identifier,zone_name,restriction,source");
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
        event.inside ? QStringLiteral("entered") : QStringLiteral("approaching"),
        number(event.distance, 0),
        csvField(event.zoneIdentifier),
        csvField(event.zoneName),
        QString::fromLatin1(restrictions.valueToKey(event.restriction)),
        csvField(event.sourceText),
    }.join(QLatin1Char(','));
}

void GeoZoneAlertLog::append(const GeoZoneMonitor::AlertEvent &event)
{
    if (!SettingsManager::instance()->geoZoneSettings()->logAlerts()->rawValue().toBool()) {
        return;
    }

    const QString path = filePath(event.time.toUTC().date());
    if (path.isEmpty()) {
        return;
    }
    (void) QDir().mkpath(QFileInfo(path).absolutePath());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        qCWarning(GeoZoneMonitorLog) << "Unable to write geozone alert log" << path << file.errorString();
        return;
    }
    QString text;
    if (file.size() == 0) {
        text = header() + QLatin1Char('\n');
    }
    text += line(event) + QLatin1Char('\n');
    (void) file.write(text.toUtf8());
}
