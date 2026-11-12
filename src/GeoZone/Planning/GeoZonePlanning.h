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
#include <QtPositioning/QGeoRectangle>

#include <optional>

#include "GeoZonePlanAudit.h"

class GeoZoneAuthorisations;

/// What mission checks plan the flight for: its start time and the authorisations the operator holds, and the geozone
/// data the open plan was saved with, see GeoZonePlanAudit
class GeoZonePlanning : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("GeoZoneAuthorisations.h")

    Q_PROPERTY(QDateTime                plannedStart    READ plannedStart   WRITE setPlannedStart   NOTIFY plannedStartChanged FINAL)  ///< Invalid to check zones whatever their schedule
    Q_PROPERTY(GeoZoneAuthorisations*   authorisations  READ authorisations                         CONSTANT FINAL)                    ///< Zones the operator is authorised to fly in
    Q_PROPERTY(QGeoRectangle            missionArea     READ missionArea                            NOTIFY missionAreaChanged FINAL)   ///< Bounds of the mission the Plan View checks, invalid without
    Q_PROPERTY(QDateTime                plannedEnd      READ plannedEnd                             NOTIFY plannedEndChanged FINAL)    ///< Estimated end of the flight from the planned start, invalid without

public:
    /// @param authorisationsFile Where the authorisations are kept
    explicit GeoZonePlanning(const QString &authorisationsFile, QObject *parent = nullptr);

    QDateTime plannedStart() const { return _plannedStart; }
    void setPlannedStart(const QDateTime &plannedStart);
    /// Checks missions against zones whatever their schedule
    Q_INVOKABLE void clearPlannedStart() { setPlannedStart(QDateTime()); }
    GeoZoneAuthorisations *authorisations() const { return _authorisations; }

    QGeoRectangle missionArea() const { return _missionArea; }
    /// Set by the mission check of the Plan View, see GeoZoneMissionCheck::missionArea
    void setMissionArea(const QGeoRectangle &missionArea);
    /// @return The estimated end of the planned flight, its start for a flight without duration, invalid without planned start
    QDateTime plannedEnd() const { return _plannedStart.isValid() ? ((_plannedEnd > _plannedStart) ? _plannedEnd : _plannedStart) : QDateTime(); }
    /// Set by the mission check of the Plan View from the mission duration, see GeoZoneMissionCheck::plannedFlightTimes
    void setPlannedEnd(const QDateTime &plannedEnd);

    /// Geozone data the open plan was saved with, nothing for plans without it, like new or older plans
    const std::optional<GeoZonePlanAudit::Record> &planAudit() const { return _planAudit; }
    void setPlanAudit(const std::optional<GeoZonePlanAudit::Record> &planAudit);
    /// @return true while the open plan has an audit record which a newer version of QGroundControl wrote and this one
    ///         can't read: the geozone data isn't compared with it
    bool planAuditUnread() const { return _planAuditUnread; }
    /// Sets whether the plan loaded has such a record, telling the pilot with planNotice when it has
    void setPlanAuditUnread(bool unread);
    /// Tells the pilot with planNotice, once, that saving the plan replaced the record this version couldn't read
    void notePlanAuditReplaced();

signals:
    void plannedStartChanged();
    void planAuditChanged();
    void missionAreaChanged();
    void plannedEndChanged();
    /// What the pilot is to know about the geozone data of the open plan, like a record this version can't read
    void planNotice(const QString &message);

private:
    QDateTime _plannedStart;
    QGeoRectangle _missionArea;
    QDateTime _plannedEnd;
    std::optional<GeoZonePlanAudit::Record> _planAudit;
    bool _planAuditUnread = false;
    GeoZoneAuthorisations *_authorisations = nullptr;
};
