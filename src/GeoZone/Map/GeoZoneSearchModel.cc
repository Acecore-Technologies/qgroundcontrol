/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneSearchModel.h"
#include "GeoZoneManager.h"

#include <QtCore/QTimer>

#include <algorithm>

GeoZoneSearchModel::GeoZoneSearchModel(QObject *parent)
    : GeoZoneSearchModel(nullptr, parent)
{
}

GeoZoneSearchModel::GeoZoneSearchModel(GeoZoneManager *manager, QObject *parent)
    : QAbstractListModel(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
    , _timer(new QTimer(this))
{
    _timer->setSingleShot(true);
    _timer->setInterval(300);
    (void) connect(_timer, &QTimer::timeout, this, &GeoZoneSearchModel::update);
    (void) connect(_timer, &QTimer::timeout, this, &GeoZoneSearchModel::pendingChanged);
    // Sources were reloaded or removed
    (void) connect(_manager, &GeoZoneManager::zonesChanged, this, &GeoZoneSearchModel::update);
}

GeoZoneSearchModel::~GeoZoneSearchModel()
{
}

void GeoZoneSearchModel::setText(const QString &text)
{
    if (text == _text) {
        return;
    }
    _text = text;
    emit textChanged();
    const bool wasPending = pending();
    _timer->start();
    if (!wasPending) {
        emit pendingChanged();
    }
}

void GeoZoneSearchModel::setMaxResults(int maxResults)
{
    // At least one result, so that hasMore never tells of more results than none listed
    maxResults = (std::max)(maxResults, minResults);
    if (maxResults != _maxResults) {
        _maxResults = maxResults;
        emit maxResultsChanged();
        update();
    }
}

bool GeoZoneSearchModel::pending() const
{
    return _timer->isActive();
}

void GeoZoneSearchModel::setDelay(int msecs)
{
    _timer->setInterval(msecs);
}

void GeoZoneSearchModel::update()
{
    const bool wasPending = pending();
    _timer->stop();

    QList<GeoZoneInfo> zones;
    // One more than listed tells that more zones match
    const QList<const GeoZone*> found = _manager ? _manager->catalog().search(_text, _maxResults + 1) : QList<const GeoZone*>();
    for (const GeoZone *zone : found) {
        if (zones.count() < _maxResults) {
            zones.append(GeoZoneInfo(zone));
        }
    }
    const bool hasMore = found.count() > _maxResults;

    beginResetModel();
    _zones = zones;
    _hasMore = hasMore;
    endResetModel();
    emit countChanged();
    if (wasPending) {
        emit pendingChanged();
    }
}

int GeoZoneSearchModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant GeoZoneSearchModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || (index.row() >= _zones.count())) {
        return QVariant();
    }
    const GeoZoneInfo &zone = _zones[index.row()];
    switch (role) {
    case ZoneRole:
        return QVariant::fromValue(zone);
    case DisplayNameRole:
        return zone.displayName();
    case IdentifierRole:
        return zone.identifier();
    case RestrictionRole:
        return zone.restriction();
    case RestrictionTextRole:
        return zone.restrictionText();
    case SourceCountRole:
        return zone.sourceCount();
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> GeoZoneSearchModel::roleNames() const
{
    return {
        { ZoneRole,             "zone" },
        { DisplayNameRole,      "displayName" },
        { IdentifierRole,       "identifier" },
        { RestrictionRole,      "restriction" },
        { RestrictionTextRole,  "restrictionText" },
        { SourceCountRole,      "sourceCount" },
    };
}
