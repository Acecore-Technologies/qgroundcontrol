/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QAbstractListModel>
#include <QtCore/QByteArray>
#include <QtCore/QDate>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtPositioning/QGeoCoordinate>

#include <optional>

#include "GeoZoneMonitor.h"

class GeoZoneManager;

/// Geozone alerts of this session, newest first, with those logged earlier today, for reviewing and acknowledging
/// them after a flight or while the alert popups are gone. Once started, the log appends the alerts to a CSV file per
/// UTC day, with what the pilot was shown and answered about them (GeoZoneMonitor::AlertEvent::pilotEvent()),
/// buffered and written to the open file every logFlushIntervalMSecs rather than opening and flushing the file for
/// each alert. The files of the days before the retention of GeoZoneSettings::recordRetention are deleted when a new
/// file is started. The application monitor always logs, the flight alerts must be traceable: lines which can't be
/// written, for lack of a folder or because the file can't be opened or written, are kept, up to maxPendingLogLines of
/// them, and written again every logRetryIntervalMSecs, while logFailing() tells the monitor and the pilot.
class GeoZoneAlertHistory : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(int              count               READ count                  NOTIFY countChanged FINAL)
    Q_PROPERTY(int              unacknowledgedCount READ unacknowledgedCount    NOTIFY unacknowledgedCountChanged FINAL)
    Q_PROPERTY(QGeoCoordinate   shownPosition       READ shownPosition          NOTIFY shownPositionChanged FINAL)  ///< Vehicle position of the alert shown on the map, invalid when none
    Q_PROPERTY(bool             logFailing          READ logFailing             NOTIFY logHealthChanged FINAL)     ///< The last alerts couldn't be written to the log, they are kept and tried again
    /// What the pilot is told about the log: why it isn't written and how many alerts wait, or how many were lost,
    /// empty while all alerts were written
    Q_PROPERTY(QString          logStatus           READ logStatus              NOTIFY logHealthChanged FINAL)

public:
    enum Roles {
        TimeRole = Qt::UserRole + 1,    ///< Local time
        MessageRole,
        ZoneNameRole,
        RestrictionRole,                ///< GeoZone::Restriction
        InsideRole,                     ///< Inside the zone: entered it, or lost the position inside it
        CoordinateRole,                 ///< Of the vehicle
        AltitudeRole,                   ///< Of the vehicle in meters AMSL, NaN when unknown
        AcknowledgedRole,
        EarlierRole,                    ///< Logged by an earlier session
        LevelRole,                      ///< GeoZoneAlertLevel::Level
    };

    /// Columns of the alert log, in the order written, see logColumnName(). New columns go last: logs written before
    /// them are still read.
    enum LogColumn {
        TimeColumn,                 ///< UTC, ISO 8601 with milliseconds
        VehicleColumn,
        LatitudeColumn,
        LongitudeColumn,
        AltitudeColumn,             ///< Meters AMSL
        TerrainColumn,              ///< Meters AMSL
        EventColumn,                ///< See GeoZoneMonitor::AlertEvent::Kind
        DistanceColumn,             ///< Meters to the zone
        ZoneIdentifierColumn,
        ZoneNameColumn,
        RestrictionColumn,
        SourceColumn,
        TimeToEntryColumn,          ///< Seconds
        TrafficColumn,
        AlertIdColumn,
        DetailColumn,
        WarningDistanceColumn,      ///< The alert thresholds, see GeoZoneMonitor::AlertEvent::warningDistance
        WarningVerticalColumn,
        WarningTimeColumn,
        GroundSpeedColumn,
        VerticalSpeedColumn,
        LevelColumn,
        LogColumnCount,
    };
    /// Columns every line of a log has, also those written by earlier versions
    static constexpr int minLogColumns = SourceColumn + 1;

    static constexpr int maxEntries = 500;
    static constexpr int shownPositionMSecs = 2 * 60 * 1000;   ///< How long the map shows the position of a shown alert
    static constexpr int logFlushIntervalMSecs = 2000;
    static constexpr int logRetryIntervalMSecs = 10000;     ///< Between two tries to write lines which couldn't be written
    static constexpr int maxPendingLogLines = 2000;         ///< Lines kept while they can't be written, the oldest are lost beyond

    /// @param manager Zones the alerts are shown on, GeoZoneManager::instance() when null
    explicit GeoZoneAlertHistory(GeoZoneManager *manager = nullptr, QObject *parent = nullptr);
    /// Writes the buffered alerts
    ~GeoZoneAlertHistory() override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(_entries.count()); }
    int unacknowledgedCount() const;
    QGeoCoordinate shownPosition() const { return _shownPosition; }

    /// Logs the alerts appended from now on, after adding those logged earlier today as acknowledged entries
    /// @param directory Folder of the log files, the telemetry folder of the application settings when empty
    void startLog(const QString &directory = QString());
    /// @return Log file for the date, empty without a folder
    QString logFilePath(const QDate &date) const;
    /// Writes the buffered alerts now. Those which can't be written are kept and tried again.
    /// @return true when none is left to write
    bool flushLog();
    bool logFailing() const { return !_logError.isEmpty(); }
    /// Why the log can't be written, empty while it is
    QString logError() const { return _logError; }
    /// Lines waiting to be written
    int pendingLogLines() const { return static_cast<int>(_logLines.count()); }
    /// Lines lost since the log started, because more than maxPendingLogLines couldn't be written
    int lostLogLines() const { return _lostLogLines; }
    QString logStatus() const;
    /// Deletes the log files of the folder for the days before the last ones, today included, none when days is 0
    /// @return Number of files deleted
    int removeOldLogs(const QDate &today, int days);

    /// The pilot acknowledged the alert of the row, or all alerts, in the list of alerts: logged, see pilotAcknowledged
    Q_INVOKABLE void acknowledge(int row);
    Q_INVOKABLE void acknowledgeAll();
    /// Acknowledges the alert by GeoZoneMonitor::AlertEvent::id, for the monitor, which logs what the pilot answered
    void acknowledgeAlert(int alertId);
    /// Shows the zone of the alert with its details and the vehicle position on the visible map, which acknowledges
    /// it like acknowledge()
    Q_INVOKABLE void show(int row);
    Q_INVOKABLE void clearShownPosition();

    /// @return Name of the column in the header of the log, like "zone_identifier"
    static QString logColumnName(LogColumn column);
    static QString logHeader();
    static QString logLine(const GeoZoneMonitor::AlertEvent &event);
    /// @return The fields of a line of a log file, unquoted
    static QStringList logFields(const QString &line);
    /// @return The alert of a line of a log file, nothing when it can't be read
    static std::optional<GeoZoneMonitor::AlertEvent> parseLogLine(const QString &line);

public slots:
    /// Adds the alert, and logs it within logFlushIntervalMSecs once the log is started
    void append(const GeoZoneMonitor::AlertEvent &event);
    /// Logs what the pilot was shown or answered about an alert, which isn't added to the alerts
    void logPilotEvent(const GeoZoneMonitor::AlertEvent &event);

signals:
    void countChanged();
    void unacknowledgedCountChanged();
    /// An alert of the session was acknowledged, by GeoZoneMonitor::AlertEvent::id
    void alertAcknowledged(int alertId);
    /// The pilot acknowledged the alert here rather than in the alert popup, which GeoZoneMonitor logs as dismissed
    /// @param detail What the pilot pressed, like "Acknowledge all"
    void pilotAcknowledged(const GeoZoneMonitor::AlertEvent &alert, const QString &detail);
    void allAcknowledged();
    void shownPositionChanged();
    /// logFailing() or logStatus() changed
    void logHealthChanged();

private:
    struct Entry {
        GeoZoneMonitor::AlertEvent event;
        bool acknowledged = false;
        bool earlier = false;
    };

    void _loadLog(const QString &filePath);
    /// @param pilotAction What the pilot pressed, empty when the monitor acknowledges
    void _acknowledge(int row, const QString &pilotAction);
    /// Logs the event within logFlushIntervalMSecs once the log is started
    void _log(const GeoZoneMonitor::AlertEvent &event);
    /// Opens the log file of the date, starting it with the header
    /// @return Why it can't be opened or started, empty when it is open
    QString _openLog(const QString &path, const QDate &date);
    /// @return Why the buffered lines can't all be written, empty when they were
    QString _writeLog();

    GeoZoneManager *_manager = nullptr;
    QList<Entry> _entries;          ///< Newest first
    QGeoCoordinate _shownPosition;
    QTimer _shownTimer;

    bool _logging = false;          ///< Set by startLog()
    QString _logDirectory;
    mutable QString _settingsDirectory; ///< The telemetry folder of the settings as read last, used without _logDirectory
    bool _closing = false;          ///< Being destroyed: the settings of the application may be gone already
    /// A line not written yet
    struct LogLine {
        QDate date;                 ///< UTC, of its file
        QByteArray text;            ///< With its line break
    };

    QFile _logFile;                 ///< Kept open between flushes
    QList<LogLine> _logLines;       ///< Not written yet, the oldest first, at most maxPendingLogLines
    QString _logError;              ///< Why the last flush failed, empty when it didn't
    int _lostLogLines = 0;
    bool _logLineTorn = false;      ///< A line was written in part, the next one starts on a new line
    QTimer _logFlushTimer;          ///< Also retries
};
