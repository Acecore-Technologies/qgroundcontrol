/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneRefreshScheduler.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneProviders.h"
#include "GeoZoneSource.h"

#include <QtCore/QLocale>
#include <QtCore/QRandomGenerator>
#include <QtCore/QTimeZone>
#include <QtCore/QTimer>
#include <QtCore/QUrl>

#include <algorithm>
#include <cmath>

GeoZoneRefreshScheduler::GeoZoneRefreshScheduler(const SourceList &sources, QObject *parent)
    : QObject(parent)
    , _sources(sources)
    , _timer(new QTimer(this))
{
    _timer->setInterval(checkIntervalMSecs);
    (void) connect(_timer, &QTimer::timeout, this, &GeoZoneRefreshScheduler::check);
}

void GeoZoneRefreshScheduler::start()
{
    _timer->start();
}

void GeoZoneRefreshScheduler::check()
{
    if (!_sources) {
        return;
    }
    const QDateTime now = GeoZoneEnvironment::currentDateTimeUtc();
    for (GeoZoneSource *const source : _sources()) {
        if (!source->busy() && isDue(*source, now) && !waitingForNetwork(*source, now)) {
            emit refreshDue(source);
        }
    }
    emit checked();
}

bool GeoZoneRefreshScheduler::_needsNetwork(const GeoZoneSource &source)
{
    return source.isFeed() && !QUrl(source.url()).isLocalFile();
}

bool GeoZoneRefreshScheduler::waitingForNetwork(const GeoZoneSource &source, const QDateTime &now) const
{
    return (_network == Network::Offline) && _needsNetwork(source) && isDue(source, now);
}

void GeoZoneRefreshScheduler::setNetwork(Network network)
{
    if (network == _network) {
        return;
    }
    _network = network;
    qCDebug(GeoZoneSourceLog) << "Network" << ((network == Network::Online) ? "online" : ((network == Network::Offline) ? "offline" : "unknown"));
    if (network != Network::Online) {
        emit networkChanged();
        return;
    }
    // The refreshes which failed while the network was down don't wait for their retry: they are due now, also those
    // the schedule didn't ask for, like a refresh of the user or the download of another area
    const QDateTime now = GeoZoneEnvironment::currentDateTimeUtc();
    for (State &state : _states) {
        if (state.unreachable) {
            state.retryAt = now;
        }
    }
    check();
    emit networkChanged();
    emit networkRestored();
}

int GeoZoneRefreshScheduler::refreshIntervalSecs(const GeoZoneSource &source) const
{
    if (!source.isFeed()) {
        return 0;
    }
    const GeoZoneEnvironment::SourceSettings settings = GeoZoneEnvironment::sourceSettings();
    int interval = source.isLive() ? (settings.liveFeedRefreshMinutes * 60) : (settings.feedRefreshHours * 3600);
    // Feeds of the catalogue may be refreshed more often, like datasets taking effect on their own dates
    if (const double catalogueHours = GeoZoneProviders::refreshHours(source.url()); catalogueHours > 0) {
        interval = (std::min)(interval, (std::max)(minRefreshSecs, static_cast<int>(std::lround(catalogueHours * 3600))));
    }
    // Servers telling caches to keep their data for a long time don't delay the refresh the user asked for
    const qint64 maxAge = _states.value(source.key()).maxAgeSecs;
    if (maxAge <= 0) {
        return interval;
    }
    return static_cast<int>((std::min)(qint64{ interval }, (std::max)(maxAge, qint64{ minRefreshSecs })));
}

QDateTime GeoZoneRefreshScheduler::nextRefresh(const GeoZoneSource &source) const
{
    if (!source.isFeed()) {
        return QDateTime();
    }
    const QDateTime retryAt = _states.value(source.key()).retryAt;
    if (retryAt.isValid()) {
        return retryAt;
    }
    const QDateTime last = source.checkedAt();
    return last.isValid() ? last.addSecs(refreshIntervalSecs(source)) : QDateTime::fromSecsSinceEpoch(0, QTimeZone::UTC);
}

bool GeoZoneRefreshScheduler::isDue(const GeoZoneSource &source, const QDateTime &now) const
{
    const QDateTime next = nextRefresh(source);
    return next.isValid() && (next <= now);
}

void GeoZoneRefreshScheduler::refreshSucceeded(const QString &key, qint64 maxAgeSecs)
{
    State &state = _states[key];
    state.failureCount = 0;
    state.retryAt = QDateTime();
    state.maxAgeSecs = maxAgeSecs;
    state.unreachable = false;
}

void GeoZoneRefreshScheduler::refreshFailed(const GeoZoneSource &source, bool unreachable)
{
    // Failed refreshes are retried later and later, so an unavailable service isn't asked every few minutes
    State &state = _states[source.key()];
    state.unreachable = unreachable;
    state.failureCount++;
    const qint64 maxDelay = (std::max)(qint64{ minRetrySecs }, (std::min)(qint64{ unreachable ? maxUnreachableRetrySecs : maxRetrySecs }, qint64{ refreshIntervalSecs(source) }));
    const qint64 delay = (std::min)(maxDelay, qint64{ minRetrySecs } << (std::min)(state.failureCount - 1, 16));
    const double jitter = 0.8 + (0.4 * QRandomGenerator::global()->generateDouble());
    state.retryAt = GeoZoneEnvironment::currentDateTimeUtc().addSecs(std::llround(delay * jitter));
}

void GeoZoneRefreshScheduler::retryWithin(const QString &key, int secs)
{
    const auto it = _states.find(key);
    if (it == _states.end()) {
        return;
    }
    const QDateTime latest = GeoZoneEnvironment::currentDateTimeUtc().addSecs(secs);
    if (it->retryAt.isValid() && (it->retryAt > latest)) {
        it->retryAt = latest;
    }
}

qint64 GeoZoneRefreshScheduler::maxAgeSecsOf(const QHash<QByteArray, QByteArray> &headers)
{
    const auto httpDate = [](const QByteArray &value) {
        const QDateTime parsed = QLocale::c().toDateTime(QString::fromLatin1(value.trimmed()), QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
        return parsed.isValid() ? QDateTime(parsed.date(), parsed.time(), QTimeZone::UTC) : QDateTime();
    };

    qint64 lifetime = -1;
    bool hasMaxAge = false;
    for (const QByteArray &directive : headers.value("cache-control").split(',')) {
        const QByteArray trimmed = directive.trimmed().toLower();
        if (trimmed.startsWith("max-age=")) {
            bool ok = false;
            const qint64 value = trimmed.mid(8).replace('"', QByteArray()).toLongLong(&ok);
            if (ok) {
                lifetime = value;
                hasMaxAge = true;
            }
        }
    }
    if (const QByteArray expires = headers.value("expires"); !hasMaxAge && !expires.isEmpty()) {
        // Relative to the server's Date, so that the clocks needn't agree
        const QDateTime expiresAt = httpDate(expires);
        const QByteArray date = headers.value("date");
        const QDateTime sentAt = date.isEmpty() ? GeoZoneEnvironment::currentDateTimeUtc() : httpDate(date);
        if (expiresAt.isValid() && sentAt.isValid()) {
            lifetime = sentAt.secsTo(expiresAt);
        }
    }
    if (lifetime > 0) {
        lifetime -= headers.value("age").trimmed().toLongLong();
    }
    return (lifetime > 0) ? lifetime : -1;
}

void GeoZoneRefreshScheduler::remove(const QString &key)
{
    (void) _states.remove(key);
}
