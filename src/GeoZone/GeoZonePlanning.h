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
#include <QtCore/QObject>

class GeoZoneAuthorisations;

/// What mission checks plan the flight for: its start time and the authorisations the operator holds
class GeoZonePlanning : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("GeoZoneAuthorisations.h")

    Q_PROPERTY(QDateTime                plannedStart    READ plannedStart   WRITE setPlannedStart   NOTIFY plannedStartChanged FINAL)  ///< Invalid to check zones whatever their schedule
    Q_PROPERTY(GeoZoneAuthorisations*   authorisations  READ authorisations                         CONSTANT FINAL)                    ///< Zones the operator is authorised to fly in

public:
    /// @param authorisationsFile Where the authorisations are kept
    explicit GeoZonePlanning(const QString &authorisationsFile, QObject *parent = nullptr);

    QDateTime plannedStart() const { return _plannedStart; }
    void setPlannedStart(const QDateTime &plannedStart);
    /// Checks missions against zones whatever their schedule
    Q_INVOKABLE void clearPlannedStart() { setPlannedStart(QDateTime()); }
    GeoZoneAuthorisations *authorisations() const { return _authorisations; }

signals:
    void plannedStartChanged();

private:
    QDateTime _plannedStart;
    GeoZoneAuthorisations *_authorisations = nullptr;
};
