/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneAlertModel.h"


#include <algorithm>
#include <cmath>

namespace {

bool sameValue(double a, double b)
{
    return (a == b) || (std::isnan(a) && std::isnan(b));
}

} // namespace

bool GeoZoneAlertModel::Alert::actionsDiffer(const Alert &other) const
{
    return (question != other.question) || (activeVehicle != other.activeVehicle) || (inside != other.inside) ||
           !sameValue(climbHeight, other.climbHeight);
}

GeoZoneAlertModel::GeoZoneAlertModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int GeoZoneAlertModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant GeoZoneAlertModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid)) {
        return QVariant();
    }
    const Alert &alert = _alerts[index.row()];
    switch (role) {
    case AlertIdRole:
        return alert.alertId;
    case MessageRole:
        return alert.message;
    case SeverityRole:
        return alert.severity;
    case ZoneKeyRole:
        return alert.zoneKey;
    case QuestionRole:
        return alert.question;
    case ActiveVehicleRole:
        return alert.activeVehicle;
    case InsideRole:
        return alert.inside;
    case ClimbHeightRole:
        return alert.climbHeight;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> GeoZoneAlertModel::roleNames() const
{
    return {
        { AlertIdRole, "alertId" },
        { MessageRole, "message" },
        { SeverityRole, "severity" },
        { ZoneKeyRole, "zoneKey" },
        { QuestionRole, "question" },
        { ActiveVehicleRole, "activeVehicle" },
        { InsideRole, "inside" },
        { ClimbHeightRole, "climbHeight" },
    };
}

bool GeoZoneAlertModel::show(const Alert &newAlert)
{
    const int previousCount = count();
    Alert alert = newAlert;
    if (!alert.zoneKey.isEmpty() && !alert.zoneKeys.contains(alert.zoneKey)) {
        alert.zoneKeys.prepend(alert.zoneKey);
    }
    const bool permission = alert.asksPermission && (alert.vehicleKey > 0) && !alert.zoneKey.isEmpty();
    for (int row = count() - 1; row >= 0; row--) {
        const Alert &shown = _alerts[row];
        const bool sameZone = permission && shown.asksPermission && (shown.vehicleKey == alert.vehicleKey) && shown.zoneKeys.contains(alert.zoneKey);
        if ((shown.message == alert.message) || ((alert.alertId > 0) && (shown.alertId == alert.alertId)) || sameZone) {
            if (shown.vehicleKey == alert.vehicleKey) {
                for (const QString &zoneKey : shown.zoneKeys) {
                    if (!alert.zoneKeys.contains(zoneKey)) {
                        alert.zoneKeys.append(zoneKey);
                    }
                }
            }
            beginRemoveRows(QModelIndex(), row, row);
            _alerts.removeAt(row);
            endRemoveRows();
        }
    }

    const auto position = std::upper_bound(_alerts.cbegin(), _alerts.cend(), alert.severity, [](int severity, const Alert &shown) {
        return severity < shown.severity;
    });
    const int row = static_cast<int>(std::distance(_alerts.cbegin(), position));
    const bool shown = row < maxAlerts;
    if (shown) {
        beginInsertRows(QModelIndex(), row, row);
        _alerts.insert(row, alert);
        endInsertRows();
        if (count() > maxAlerts) {
            beginRemoveRows(QModelIndex(), maxAlerts, count() - 1);
            _alerts.resize(maxAlerts);
            endRemoveRows();
        }
    }
    _updateSummary();
    if (count() != previousCount) {
        emit countChanged();
    }
    if (shown) {
        emit alertShown();
    }
    return shown;
}

void GeoZoneAlertModel::removeIf(const std::function<bool(const Alert &)> &predicate)
{
    const int previousCount = count();
    for (int row = count() - 1; row >= 0; row--) {
        if (predicate(_alerts[row])) {
            beginRemoveRows(QModelIndex(), row, row);
            _alerts.removeAt(row);
            endRemoveRows();
        }
    }
    if (count() != previousCount) {
        _updateSummary();
        emit countChanged();
    }
}

void GeoZoneAlertModel::clear()
{
    removeIf([](const Alert &) { return true; });
}

void GeoZoneAlertModel::updateActions(const std::function<void(Alert &)> &update)
{
    for (int row = 0; row < count(); row++) {
        Alert alert = _alerts[row];
        update(alert);
        _setActions(row, alert);
    }
    _updateSummary();
}

void GeoZoneAlertModel::_setActions(int row, const Alert &alert)
{
    Alert &current = _alerts[row];
    if (!current.actionsDiffer(alert)) {
        return;
    }
    QList<int> roles;
    if (current.question != alert.question) {
        roles.append(QuestionRole);
    }
    if (current.activeVehicle != alert.activeVehicle) {
        roles.append(ActiveVehicleRole);
    }
    if (current.inside != alert.inside) {
        roles.append(InsideRole);
    }
    if (!sameValue(current.climbHeight, alert.climbHeight)) {
        roles.append(ClimbHeightRole);
    }
    current.question = alert.question;
    current.activeVehicle = alert.activeVehicle;
    current.inside = alert.inside;
    current.climbHeight = alert.climbHeight;
    emit dataChanged(index(row), index(row), roles);
}

void GeoZoneAlertModel::_updateSummary()
{
    const bool active = std::any_of(_alerts.cbegin(), _alerts.cend(), [](const Alert &alert) { return alert.activeVehicle; });
    if (active != _activeVehicleAlerts) {
        _activeVehicleAlerts = active;
        emit activeVehicleAlertsChanged();
    }
    const int questions = static_cast<int>(std::count_if(_alerts.cbegin(), _alerts.cend(), [](const Alert &alert) { return alert.question; }));
    if (questions != _questionCount) {
        _questionCount = questions;
        emit questionCountChanged();
    }
    // Above the highest ceiling, which is above the others
    double climb = qQNaN();
    for (const Alert &alert : std::as_const(_alerts)) {
        if (alert.activeVehicle && (alert.climbHeight > 0)) {
            climb = std::isnan(climb) ? alert.climbHeight : (std::max)(climb, alert.climbHeight);
        }
    }
    if (!sameValue(climb, _climbHeight)) {
        _climbHeight = climb;
        emit climbHeightChanged();
    }
}
