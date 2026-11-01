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
#include "GeoZoneSource.h"

#include <QtCore/QRandomGenerator>
#include <QtCore/QTimeZone>
#include <QtCore/QTimer>

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
        if (!source->busy() && isDue(*source, now)) {
            emit refreshDue(source);
        }
    }
}

int GeoZoneRefreshScheduler::refreshIntervalSecs(const GeoZoneSource &source) const
{
    if (!source.isFeed()) {
        return 0;
    }
    const GeoZoneEnvironment::SourceSettings settings = GeoZoneEnvironment::sourceSettings();
    const int interval = source.isLive() ? (settings.liveFeedRefreshMinutes * 60) : (settings.feedRefreshHours * 3600);
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
    const QDateTime last = source.loadedAt();
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
}

void GeoZoneRefreshScheduler::refreshFailed(const GeoZoneSource &source)
{
    // Failed refreshes are retried later and later, so an unavailable service isn't asked every few minutes
    State &state = _states[source.key()];
    state.failureCount++;
    const qint64 maxDelay = (std::max)(qint64{ minRetrySecs }, (std::min)(qint64{ maxRetrySecs }, qint64{ refreshIntervalSecs(source) }));
    const qint64 delay = (std::min)(maxDelay, qint64{ minRetrySecs } << (std::min)(state.failureCount - 1, 16));
    const double jitter = 0.8 + (0.4 * QRandomGenerator::global()->generateDouble());
    state.retryAt = GeoZoneEnvironment::currentDateTimeUtc().addSecs(std::llround(delay * jitter));
}

void GeoZoneRefreshScheduler::remove(const QString &key)
{
    (void) _states.remove(key);
}
