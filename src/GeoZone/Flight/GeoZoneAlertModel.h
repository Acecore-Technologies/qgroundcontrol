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
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QtNumeric>

#include <functional>

/// Alerts shown by the alert popup, the most severe first, with what can be done about them. GeoZoneMonitor adds the
/// alerts it shows and repeats, and updates their actions after each evaluation. Rows are notified only when their
/// values change, so the popup keeps its delegates.
class GeoZoneAlertModel : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(int  count               READ count                  NOTIFY countChanged FINAL)
    Q_PROPERTY(int  questionCount       READ questionCount          NOTIFY questionCountChanged FINAL)          ///< Alerts asking for permission, unanswered
    Q_PROPERTY(bool activeVehicleAlerts READ activeVehicleAlerts    NOTIFY activeVehicleAlertsChanged FINAL)    ///< Some alert is about the active vehicle
    /// Meters the active vehicle climbs to be above the ceilings of the zones of its alerts which have one, NaN without
    Q_PROPERTY(double climbHeight       READ climbHeight            NOTIFY climbHeightChanged FINAL)

public:
    struct Alert {
        int alertId = 0;            ///< Unique for the session, see GeoZoneMonitor::AlertEvent::id
        QString message;
        int severity = 0;           ///< Lower is more severe
        int vehicleKey = 0;         ///< GeoZoneVehicleMonitor::key of the vehicle, 0 for traffic and fence breaches
        QString zoneKey;            ///< GeoZone::key, empty for fence breaches

        // Actions, see GeoZoneMonitor
        bool question = false;      ///< Asks whether the pilot may fly in the zone, while the vehicle is in or near it
        bool activeVehicle = false; ///< About the active vehicle
        bool inside = false;        ///< The vehicle is inside the zone, by its current position
        double climbHeight = qQNaN();   ///< See GeoZoneMonitor::climbClearance, NaN when the vehicle can't climb out

        /// @return true when the actions differ
        bool actionsDiffer(const Alert &other) const;
    };

    enum Roles {
        AlertIdRole = Qt::UserRole + 1,
        MessageRole,
        SeverityRole,
        ZoneKeyRole,
        QuestionRole,
        ActiveVehicleRole,
        InsideRole,
        ClimbHeightRole,
    };

    static constexpr int maxAlerts = 4;

    explicit GeoZoneAlertModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(_alerts.count()); }
    int questionCount() const { return _questionCount; }
    const QList<Alert> &alerts() const { return _alerts; }
    bool activeVehicleAlerts() const { return _activeVehicleAlerts; }
    double climbHeight() const { return _climbHeight; }

    /// Adds the alert after those as severe, replacing a shown alert with the same id or message, and keeps the
    /// maxAlerts most severe. Emits alertShown() when it is among them.
    /// @return true when the alert is shown
    bool show(const Alert &alert);
    /// Removes the alerts matching the predicate
    void removeIf(const std::function<bool(const Alert &)> &predicate);
    /// Updates the actions of the alerts, notifying the rows which changed
    void updateActions(const std::function<void(Alert &)> &update);

    /// Removes all alerts, like the popup does when it closes by itself
    Q_INVOKABLE void clear();

signals:
    void countChanged();
    void questionCountChanged();
    void activeVehicleAlertsChanged();
    void climbHeightChanged();
    /// An alert was added or replaced a shown one
    void alertShown();

private:
    void _setActions(int row, const Alert &alert);
    void _updateSummary();

    QList<Alert> _alerts;
    int _questionCount = 0;
    bool _activeVehicleAlerts = false;
    double _climbHeight = qQNaN();
};
