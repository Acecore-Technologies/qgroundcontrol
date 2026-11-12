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
/// a failed refresh is retried later and later. While the network is known to be down, refreshes wait for it rather
/// than fail, and refreshes which failed because no server could be reached are retried as soon as it is back. Files
/// are only reloaded by the user. Times are those of GeoZoneEnvironment::currentDateTimeUtc, so that tests can move
/// the clock.
class GeoZoneRefreshScheduler : public QObject
{
    Q_OBJECT

public:
    using SourceList = std::function<QList<GeoZoneSource*>()>;

    /// Whether the network is up, from QNetworkInformation
    enum class Network : quint8 {
        Unknown,    ///< Not known, or only a local network: refreshes are tried
        Online,
        Offline,    ///< Refreshes of downloaded feeds wait for the network
    };

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
    /// @return When the source is due: after the refresh interval from when the server last confirmed its data, or
    ///         when its failed refresh is retried. Invalid for files.
    QDateTime nextRefresh(const GeoZoneSource &source) const;
    bool isDue(const GeoZoneSource &source, const QDateTime &now) const;

    /// Records a load or refresh of a source which succeeded, the retries of failed refreshes end
    /// @param maxAgeSecs How long the server said the data stays current, see maxAgeSecsOf
    void refreshSucceeded(const QString &key, qint64 maxAgeSecs);
    /// Records a failed refresh, retried after minRetrySecs, then twice as long after each further failure up to
    /// maxRetrySecs or the refresh interval when shorter, within 20 % so that clients don't retry together
    /// @param unreachable No server answered, see GeoZoneDownloader::Result::unreachable: the refresh is retried when
    ///                    the network comes back, and at least every maxUnreachableRetrySecs
    void refreshFailed(const GeoZoneSource &source, bool unreachable = false);
    /// Retries a failed refresh within the seconds when its retry is later, like the zones around a vehicle in flight
    void retryWithin(const QString &key, int secs);
    void remove(const QString &key);

    Network network() const { return _network; }
    /// Sets whether the network is up. When it comes back, the refreshes which failed for lack of a network are
    /// retried at once with the feeds which became due meanwhile, without waiting for their retry.
    void setNetwork(Network network);
    /// @return true when the source is due but waits for the network
    bool waitingForNetwork(const GeoZoneSource &source, const QDateTime &now) const;
    /// @return true when the last refresh of the source failed for lack of a network
    bool unreachable(const QString &key) const { return _states.value(key).unreachable; }

    /// @return Earliest refresh after a failed one, invalid when the last refresh succeeded
    QDateTime retryAt(const QString &key) const { return _states.value(key).retryAt; }
    /// @return How long the server said the data of the source stays current, negative when it didn't
    qint64 maxAgeSecs(const QString &key) const { return _states.value(key).maxAgeSecs; }
    /// @return Seconds a response stays current from its headers (by lower case name): Cache-Control max-age, or
    ///         Expires after Date, less its Age. Negative when the response doesn't tell, or tells to revalidate it at once.
    static qint64 maxAgeSecsOf(const QHash<QByteArray, QByteArray> &headers);

    static constexpr int minRetrySecs = 5 * 60;         ///< After the first failed refresh, doubling with each failure
    static constexpr int maxRetrySecs = 6 * 3600;       ///< Or the refresh interval when shorter
    /// Longest retry of refreshes which failed because no server answered, which cost the servers nothing: a feed
    /// missed offline is refreshed this soon after the network is back, also when the platform doesn't tell
    static constexpr int maxUnreachableRetrySecs = 15 * 60;
    static constexpr int minRefreshSecs = 5 * 60;       ///< Shortest refresh interval a server sets
    static constexpr int checkIntervalMSecs = 5 * 60 * 1000;

signals:
    /// The feed should be downloaded again
    void refreshDue(GeoZoneSource *source);
    /// check() looked at the sources, for other periodic checks
    void checked();
    /// The network came back, see setNetwork
    void networkRestored();
    void networkChanged();

private:
    struct State {
        int failureCount = 0;   ///< Refreshes failed in a row
        QDateTime retryAt;
        qint64 maxAgeSecs = -1;
        bool unreachable = false;   ///< The last refresh failed because no server answered
    };

    /// @return true for feeds downloaded from a server, which need the network
    static bool _needsNetwork(const GeoZoneSource &source);

    SourceList _sources;
    Network _network = Network::Unknown;
    QTimer *_timer = nullptr;
    QHash<QString, State> _states;  ///< By source key
};
