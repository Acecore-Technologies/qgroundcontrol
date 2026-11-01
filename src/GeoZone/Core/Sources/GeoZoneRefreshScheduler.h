/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>

#include <functional>

class GeoZoneSource;
class QTimer;

/// Decides when the geozone feeds are downloaded again, and asks for it. A feed is refreshed after the refresh interval
/// of GeoZoneEnvironment::sourceSettings, sooner when the server said its data stays current for a shorter time, and
/// a failed refresh is retried later and later. Files are only reloaded by the user. Times are those of
/// GeoZoneEnvironment::currentDateTimeUtc, so that tests can move the clock.
class GeoZoneRefreshScheduler : public QObject
{
    Q_OBJECT

public:
    using SourceList = std::function<QList<GeoZoneSource*>()>;

    /// @param sources The sources check() looks at
    explicit GeoZoneRefreshScheduler(const SourceList &sources = SourceList(), QObject *parent = nullptr);

    /// Checks the sources every checkIntervalMSecs from now on
    void start();
    /// Emits refreshDue for each source which is due and not busy
    void check();

    /// @return Seconds after which a feed is downloaded again: the refresh interval of the settings, or the shorter time
    ///         the server said its data stays current, but at least minRefreshSecs. 0 for files, which only the user
    ///         reloads.
    int refreshIntervalSecs(const GeoZoneSource &source) const;
    /// @return When the source is due: after the refresh interval from its last download, or when its failed refresh
    ///         is retried. Invalid for files.
    QDateTime nextRefresh(const GeoZoneSource &source) const;
    bool isDue(const GeoZoneSource &source, const QDateTime &now) const;

    /// Records a load or refresh of a source which succeeded, the retries of failed refreshes end
    /// @param maxAgeSecs How long the server said the data stays current, see GeoZoneDownloader::Result::maxAgeSecs
    void refreshSucceeded(const QString &key, qint64 maxAgeSecs);
    /// Records a failed refresh, retried after minRetrySecs, then twice as long after each further failure up to
    /// maxRetrySecs or the refresh interval when shorter, within 20 % so that clients don't retry together
    void refreshFailed(const GeoZoneSource &source);
    void remove(const QString &key);

    /// @return Earliest refresh after a failed one, invalid when the last refresh succeeded
    QDateTime retryAt(const QString &key) const { return _states.value(key).retryAt; }
    /// @return How long the server said the data of the source stays current, negative when it didn't
    qint64 maxAgeSecs(const QString &key) const { return _states.value(key).maxAgeSecs; }

    static constexpr int minRetrySecs = 5 * 60;         ///< After the first failed refresh, doubling with each failure
    static constexpr int maxRetrySecs = 6 * 3600;       ///< Or the refresh interval when shorter
    static constexpr int minRefreshSecs = 5 * 60;       ///< Shortest refresh interval a server sets
    static constexpr int checkIntervalMSecs = 5 * 60 * 1000;

signals:
    /// The feed should be downloaded again
    void refreshDue(GeoZoneSource *source);

private:
    struct State {
        int failureCount = 0;   ///< Refreshes failed in a row
        QDateTime retryAt;
        qint64 maxAgeSecs = -1;
    };

    SourceList _sources;
    QTimer *_timer = nullptr;
    QHash<QString, State> _states;  ///< By source key
};
