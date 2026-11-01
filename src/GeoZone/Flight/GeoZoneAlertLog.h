/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDate>
#include <QtCore/QFile>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>

#include "GeoZoneMonitor.h"

/// Appends geozone alerts to a CSV file per UTC day, for reviewing flights. The alerts are buffered and written to the
/// open file every flushIntervalMSecs, rather than opening and flushing the file for each alert. The files of days
/// older than the retention of GeoZoneSettings are deleted when a new file is started. Logging is turned on and off in
/// GeoZoneSettings.
class GeoZoneAlertLog : public QObject
{
    Q_OBJECT

public:
    static constexpr int flushIntervalMSecs = 2000;

    explicit GeoZoneAlertLog(QObject *parent = nullptr);
    /// Writes the buffered alerts
    ~GeoZoneAlertLog() override;

    /// @param directory Folder of the log files. Empty for the telemetry folder of the application settings.
    void setDirectory(const QString &directory);

    /// @return Log file for the date, empty without a folder
    QString filePath(const QDate &date) const;

    /// Deletes the log files of the folder for the days before the last ones, today included
    /// @return Number of files deleted
    int removeOldFiles(const QDate &today, int days);

    static QString header();
    static QString line(const GeoZoneMonitor::AlertEvent &event);

public slots:
    /// Buffers the alert, written within flushIntervalMSecs
    void append(const GeoZoneMonitor::AlertEvent &event);
    /// Writes the buffered alerts now
    void flush();

private:
    /// Opens the log file the buffer belongs to, starting it with the header
    bool _open();

    QString _directory;
    QFile _file;                ///< Kept open between flushes
    QByteArray _buffer;         ///< Lines not written yet
    QString _bufferPath;        ///< File the buffered lines belong to
    QDate _bufferDate;
    QTimer _flushTimer;
};
