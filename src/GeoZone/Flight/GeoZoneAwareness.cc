/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneAwareness.h"
#include "GeoZoneIntendedLevel.h"
#include "GeoZoneManager.h"
#include "GeoZoneSource.h"
#include "GeoZoneSourceRepository.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int maxListedSources = 3;

} // namespace

QStringList GeoZoneAwareness::Status::reasons() const
{
    QStringList texts;
    for (const Issue &issue : issues) {
        texts.append(issue.text);
    }
    return texts;
}

QString GeoZoneAwareness::Status::summary() const
{
    return (state == Available) ? GeoZoneAwareness::tr("Geo-awareness available") : alertMessage(state, reasons().join(QStringLiteral("; ")));
}

bool GeoZoneAwareness::Status::has(Reason reason) const
{
    return std::any_of(issues.cbegin(), issues.cend(), [reason](const Issue &issue) { return issue.reason == reason; });
}

std::optional<GeoZoneAwareness::Tracker::Change> GeoZoneAwareness::Tracker::update(const Status &status, bool inFlight, qint64 msecs)
{
    if (!inFlight) {
        _alerted = Available;
        _pending.reset();
        return std::nullopt;
    }
    if (status.state == _alerted) {
        _pending.reset();
        return std::nullopt;
    }
    if (_pending != status.state) {
        _pending = status.state;
        _pendingMSecs = msecs;
    }
    // Short drops, like a few seconds of poor GNSS, aren't alerted
    const qint64 settle = (status.state > _alerted) ? worseSettleMSecs : betterSettleMSecs;
    if ((msecs - _pendingMSecs) < settle) {
        return std::nullopt;
    }
    const Change change{ _alerted, status.state, status };
    _alerted = status.state;
    _pending.reset();
    return change;
}

GeoZoneAwareness::GeoZoneAwareness(QObject *parent)
    : QObject(parent)
{
}

void GeoZoneAwareness::setStatus(const Status &status)
{
    if (status == _status) {
        return;
    }
    _status = status;
    emit changed();
}

GeoZoneAwareness::Status GeoZoneAwareness::evaluate(const Input &input)
{
    Status status;
    const auto add = [&status](Reason reason, State state, const QString &text) {
        status.issues.append(Issue{ reason, state, text });
        status.state = (std::max)(status.state, state);
    };

    if (input.vehicle) {
        if (input.linkLost) {
            add(Reason::LinkLost, Unavailable, tr("telemetry link lost"));
        } else {
            const Gnss &gnss = input.gnss;
            if (gnss.known && (gnss.fixType < 2)) {
                add(Reason::GnssLost, Unavailable, tr("GNSS lost (no fix)"));
            }
            if (input.positionUnavailable) {
                add(Reason::NoPosition, Unavailable, tr("no current vehicle position"));
            }
        }
    }
    if (input.zoneCount <= 0) {
        add(Reason::NoData, Unavailable, tr("no geozone data loaded"));
    }

    if (input.vehicle && !input.linkLost && input.gnss.known && (input.gnss.fixType >= 2)) {
        const Gnss &gnss = input.gnss;
        QStringList poor;
        if (gnss.fixType < GnssLimits::minFixType) {
            poor.append(tr("2D fix"));
        }
        if ((gnss.satellites >= 0) && (gnss.satellites < GnssLimits::minSatellites)) {
            poor.append(tr("%n satellite(s)", nullptr, gnss.satellites));
        }
        if (!std::isnan(gnss.hdop) && (gnss.hdop > GnssLimits::maxHdop)) {
            poor.append(tr("HDOP %1").arg(gnss.hdop, 0, 'f', 1));
        }
        if (!std::isnan(gnss.horizontalAccuracy) && (gnss.horizontalAccuracy > GnssLimits::maxHorizontalAccuracy)) {
            poor.append(tr("accuracy %1 m").arg(gnss.horizontalAccuracy, 0, 'f', 1));
        }
        if (!poor.isEmpty()) {
            add(Reason::GnssPoor, Degraded, tr("GNSS poor (%1)").arg(poor.join(QStringLiteral(", "))));
        }
    }
    if (input.zoneCount > 0) {
        if (input.dataInvalid) {
            add(Reason::DataExpired, Degraded, tr("geozone data expired"));
        } else if (!input.validityWarning.isEmpty()) {
            add(Reason::DataOutdated, Degraded, tr("geozone data may be outdated"));
        }
    }
    if (!input.failedSources.isEmpty()) {
        QStringList names = input.failedSources.mid(0, maxListedSources);
        if (input.failedSources.count() > maxListedSources) {
            names.append(tr("%n more", nullptr, static_cast<int>(input.failedSources.count() - maxListedSources)));
        }
        add(Reason::RefreshFailed, Degraded, tr("update of %1 failed").arg(names.join(QStringLiteral(", "))));
    }
    if (input.covered == false) {
        add(Reason::NotCovered, Degraded, tr("no geozone data covers the vehicle position"));
    }

    // The unavailable reasons first
    std::stable_sort(status.issues.begin(), status.issues.end(), [](const Issue &a, const Issue &b) { return a.state > b.state; });
    return status;
}

GeoZoneAwareness::Input GeoZoneAwareness::dataInput(GeoZoneManager *manager, const QGeoCoordinate &position)
{
    Input input;
    if (!manager) {
        return input;
    }
    const GeoZoneSourceRepository *const repository = manager->repository();
    input.zoneCount = manager->zoneCount();
    input.validityWarning = repository->validityWarning();
    input.dataInvalid = repository->dataInvalid();
    bool covered = false;
    for (const GeoZoneSource *const source : repository->sourceList()) {
        if (!source->enabled()) {
            continue;
        }
        if (!source->errorString().isEmpty()) {
            input.failedSources.append(source->name().isEmpty() ? source->key() : source->name());
        }
        if (position.isValid() && (source->zoneCount() > 0) &&
            (source->box().contains(position) || (source->area().isValid() && source->area().contains(position)))) {
            covered = true;
        }
    }
    if (position.isValid() && (input.zoneCount > 0)) {
        input.covered = covered;
    }
    return input;
}

QString GeoZoneAwareness::stateText(State state)
{
    switch (state) {
    case Degraded:
        return tr("Degraded");
    case Unavailable:
        return tr("Unavailable");
    case Available:
        break;
    }
    return tr("Available");
}

QString GeoZoneAwareness::stateCode(State state)
{
    switch (state) {
    case Degraded:
        return QStringLiteral("degraded");
    case Unavailable:
        return QStringLiteral("unavailable");
    case Available:
        break;
    }
    return QStringLiteral("available");
}

GeoZoneAwareness::State GeoZoneAwareness::stateFromCode(const QString &code)
{
    for (const State state : { Degraded, Unavailable }) {
        if (code == stateCode(state)) {
            return state;
        }
    }
    return Available;
}

QString GeoZoneAwareness::alertMessage(State state, const QString &reasons)
{
    switch (state) {
    case Degraded:
        return tr("Geo-awareness degraded: %1").arg(reasons);
    case Unavailable:
        return tr("Geo-awareness unavailable: %1").arg(reasons);
    case Available:
        break;
    }
    return tr("Geo-awareness available again");
}

int GeoZoneAwareness::alertLevel(State to)
{
    switch (to) {
    case Unavailable:
        return GeoZoneIntendedLevel::Warning;
    case Degraded:
        return GeoZoneIntendedLevel::Caution;
    case Available:
        break;
    }
    return GeoZoneIntendedLevel::Advisory;
}
