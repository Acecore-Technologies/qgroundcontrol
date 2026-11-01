/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneAlertHistory.h"
#include "GeoZoneInfo.h"
#include "GeoZoneManager.h"

#include <QtCore/QFile>
#include <QtCore/QMetaEnum>
#include <QtCore/QTextStream>

#include <algorithm>
#include <cmath>

namespace {

/// Splits a CSV line written by GeoZoneAlertLog, undoing its quoting
QStringList csvFields(const QString &line)
{
    QStringList fields;
    QString field;
    bool quoted = false;
    for (qsizetype i = 0; i < line.size(); i++) {
        const QChar character = line.at(i);
        if (quoted) {
            if (character == QLatin1Char('"')) {
                if ((i + 1 < line.size()) && (line.at(i + 1) == QLatin1Char('"'))) {
                    field += character;
                    i++;
                } else {
                    quoted = false;
                }
            } else {
                field += character;
            }
        } else if (character == QLatin1Char('"')) {
            quoted = true;
        } else if (character == QLatin1Char(',')) {
            fields.append(field);
            field.clear();
        } else {
            field += character;
        }
    }
    fields.append(field);

    // Text the log kept from running as a spreadsheet formula
    static const QString formulaStarts = QStringLiteral("=+-@\t\r");
    for (QString &text : fields) {
        if ((text.size() >= 2) && (text.front() == QLatin1Char('\'')) && formulaStarts.contains(text.at(1))) {
            text.remove(0, 1);
        }
    }
    return fields;
}

double number(const QString &text)
{
    bool ok = false;
    const double value = text.toDouble(&ok);
    return ok ? value : qQNaN();
}

} // namespace

GeoZoneAlertHistory::GeoZoneAlertHistory(GeoZoneManager *manager, QObject *parent)
    : QAbstractListModel(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
{
    _shownTimer.setSingleShot(true);
    _shownTimer.setInterval(shownPositionMSecs);
    (void) connect(&_shownTimer, &QTimer::timeout, this, &GeoZoneAlertHistory::clearShownPosition);
}

int GeoZoneAlertHistory::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant GeoZoneAlertHistory::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || (index.row() >= _entries.count())) {
        return QVariant();
    }
    const Entry &entry = _entries[index.row()];
    switch (role) {
    case TimeRole:
        return entry.event.time.toLocalTime();
    case MessageRole:
        return entry.event.message;
    case ZoneNameRole:
        return entry.event.zoneName.isEmpty() ? entry.event.zoneIdentifier : entry.event.zoneName;
    case RestrictionRole:
        return static_cast<int>(entry.event.restriction);
    case InsideRole:
        return entry.event.inside;
    case CoordinateRole:
        return QVariant::fromValue(entry.event.position.coordinate);
    case AltitudeRole:
        return entry.event.position.altitudeAMSL;
    case AcknowledgedRole:
        return entry.acknowledged;
    case EarlierRole:
        return entry.earlier;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> GeoZoneAlertHistory::roleNames() const
{
    return {
        { TimeRole, "time" },
        { MessageRole, "message" },
        { ZoneNameRole, "zoneName" },
        { RestrictionRole, "restriction" },
        { InsideRole, "inside" },
        { CoordinateRole, "coordinate" },
        { AltitudeRole, "altitudeAMSL" },
        { AcknowledgedRole, "acknowledged" },
        { EarlierRole, "earlier" },
    };
}

int GeoZoneAlertHistory::unacknowledgedCount() const
{
    return static_cast<int>(std::count_if(_entries.cbegin(), _entries.cend(), [](const Entry &entry) { return !entry.acknowledged; }));
}

void GeoZoneAlertHistory::append(const GeoZoneMonitor::AlertEvent &event)
{
    if (_entries.count() >= maxEntries) {
        beginRemoveRows(QModelIndex(), maxEntries - 1, static_cast<int>(_entries.count()) - 1);
        _entries.resize(maxEntries - 1);
        endRemoveRows();
    }
    beginInsertRows(QModelIndex(), 0, 0);
    _entries.prepend(Entry{ event, false, false });
    endInsertRows();
    emit countChanged();
    emit unacknowledgedCountChanged();
}

void GeoZoneAlertHistory::loadLog(const QString &filePath)
{
    QFile file(filePath);
    if (filePath.isEmpty() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    // Appended after the alerts of this session, the newest first
    QList<Entry> earlier;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        if (const std::optional<GeoZoneMonitor::AlertEvent> event = parseLogLine(stream.readLine())) {
            earlier.prepend(Entry{ *event, true, true });
        }
    }
    const qsizetype room = maxEntries - _entries.count();
    if (earlier.count() > room) {
        earlier.resize(room);
    }
    if (earlier.isEmpty()) {
        return;
    }
    beginInsertRows(QModelIndex(), static_cast<int>(_entries.count()), static_cast<int>(_entries.count() + earlier.count()) - 1);
    _entries.append(earlier);
    endInsertRows();
    emit countChanged();
}

std::optional<GeoZoneMonitor::AlertEvent> GeoZoneAlertHistory::parseLogLine(const QString &line)
{
    const QStringList fields = csvFields(line);
    if ((fields.count() < 12) || (fields[0] == QStringLiteral("time_utc"))) {
        return std::nullopt;
    }
    GeoZoneMonitor::AlertEvent event;
    event.time = QDateTime::fromString(fields[0], Qt::ISODateWithMs);
    if (fields[6] == QStringLiteral("entered")) {
        event.kind = GeoZoneMonitor::AlertEvent::Entry;
    } else if (fields[6] == QStringLiteral("position_lost")) {
        event.kind = GeoZoneMonitor::AlertEvent::PositionLost;
    } else if (fields[6] != QStringLiteral("approaching")) {
        return std::nullopt;
    }
    if (!event.time.isValid()) {
        return std::nullopt;
    }
    bool vehicleOk = false;
    const int vehicleId = fields[1].toInt(&vehicleOk);
    event.vehicleId = vehicleOk ? vehicleId : GeoZoneMonitor::noVehicle;
    event.position.coordinate = QGeoCoordinate(number(fields[2]), number(fields[3]));
    event.position.altitudeAMSL = number(fields[4]);
    event.position.terrainAMSL = number(fields[5]);
    event.distance = number(fields[7]);
    event.inside = (event.kind == GeoZoneMonitor::AlertEvent::Entry) || ((event.kind == GeoZoneMonitor::AlertEvent::PositionLost) && (event.distance == 0));
    event.zoneIdentifier = fields[8];
    event.zoneName = fields[9];
    bool restrictionOk = false;
    const int restriction = QMetaEnum::fromType<GeoZone::Restriction>().keyToValue(fields[10].toLatin1().constData(), &restrictionOk);
    event.restriction = restrictionOk ? static_cast<GeoZone::Restriction>(restriction) : GeoZone::Unknown;
    event.sourceText = fields[11];
    event.timeToEntry = (fields.count() > 12) ? number(fields[12]) : qQNaN();

    // The messages of the monitor
    const QString name = event.zoneName.isEmpty() ? event.zoneIdentifier : event.zoneName;
    event.message = GeoZoneMonitor::alertMessage(event, name, GeoZone::restrictionToString(event.restriction));
    return event;
}

void GeoZoneAlertHistory::acknowledge(int row)
{
    if ((row < 0) || (row >= _entries.count()) || _entries[row].acknowledged) {
        return;
    }
    _entries[row].acknowledged = true;
    emit dataChanged(index(row), index(row), { AcknowledgedRole });
    emit unacknowledgedCountChanged();
    if (_entries[row].event.id > 0) {
        emit alertAcknowledged(_entries[row].event.id);
    }
}

void GeoZoneAlertHistory::acknowledgeAlert(int alertId)
{
    if (alertId <= 0) {
        return;
    }
    const auto it = std::find_if(_entries.cbegin(), _entries.cend(), [alertId](const Entry &entry) { return entry.event.id == alertId; });
    if (it != _entries.cend()) {
        acknowledge(static_cast<int>(std::distance(_entries.cbegin(), it)));
    }
}

void GeoZoneAlertHistory::acknowledgeAll()
{
    if (unacknowledgedCount() == 0) {
        return;
    }
    for (Entry &entry : _entries) {
        entry.acknowledged = true;
    }
    emit dataChanged(index(0), index(count() - 1), { AcknowledgedRole });
    emit unacknowledgedCountChanged();
    emit allAcknowledged();
}

void GeoZoneAlertHistory::show(int row)
{
    if ((row < 0) || (row >= _entries.count())) {
        return;
    }
    acknowledge(row);
    const GeoZoneMonitor::AlertEvent &event = _entries[row].event;
    _shownPosition = event.position.coordinate;
    _shownTimer.start();
    emit shownPositionChanged();

    // The zone of a session alert by its key, otherwise one with its identifier and name
    const QList<GeoZone*> &zones = _manager->zoneList();
    auto zone = std::find_if(zones.cbegin(), zones.cend(), [&event](const GeoZone *candidate) { return !event.zoneKey.isEmpty() && (candidate->key() == event.zoneKey); });
    if (zone == zones.cend()) {
        zone = std::find_if(zones.cbegin(), zones.cend(), [&event](const GeoZone *candidate) {
            return (candidate->identifier() == event.zoneIdentifier) && (candidate->name() == event.zoneName);
        });
    }
    if (zone != zones.cend()) {
        _manager->showZone(QVariant::fromValue(GeoZoneInfo(*zone)));
    }
}

void GeoZoneAlertHistory::clearShownPosition()
{
    _shownTimer.stop();
    if (_shownPosition.isValid()) {
        _shownPosition = QGeoCoordinate();
        emit shownPositionChanged();
    }
}
