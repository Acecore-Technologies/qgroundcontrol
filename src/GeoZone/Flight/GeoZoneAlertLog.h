/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QDate>
#include <QtCore/QObject>
#include <QtCore/QString>

#include "GeoZoneMonitor.h"

/// Appends geozone alerts to a CSV file per UTC day, for reviewing flights. Logging is turned on and off in
/// GeoZoneSettings.
class GeoZoneAlertLog : public QObject
{
    Q_OBJECT

public:
    explicit GeoZoneAlertLog(QObject *parent = nullptr);

    /// @param directory Folder of the log files. Empty for the telemetry folder of the application settings.
    void setDirectory(const QString &directory) { _directory = directory; }

    /// @return Log file for the date, empty without a folder
    QString filePath(const QDate &date) const;

    static QString header();
    static QString line(const GeoZoneMonitor::AlertEvent &event);

public slots:
    void append(const GeoZoneMonitor::AlertEvent &event);

private:
    QString _directory;
};
