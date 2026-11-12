/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZonePlanning.h"
#include "GeoZoneAuthorisations.h"

GeoZonePlanning::GeoZonePlanning(const QString &authorisationsFile, QObject *parent)
    : QObject(parent)
    , _authorisations(new GeoZoneAuthorisations(authorisationsFile, this))
{
}

void GeoZonePlanning::setPlannedStart(const QDateTime &plannedStart)
{
    const QDateTime start = plannedStart.isValid() ? plannedStart.toUTC() : QDateTime();
    if (start != _plannedStart) {
        const QDateTime end = this->plannedEnd();
        _plannedStart = start;
        emit plannedStartChanged();
        if (this->plannedEnd() != end) {
            emit plannedEndChanged();
        }
    }
}

void GeoZonePlanning::setPlannedEnd(const QDateTime &plannedEnd)
{
    const QDateTime end = plannedEnd.isValid() ? plannedEnd.toUTC() : QDateTime();
    if (end != _plannedEnd) {
        const QDateTime previous = this->plannedEnd();
        _plannedEnd = end;
        if (this->plannedEnd() != previous) {
            emit plannedEndChanged();
        }
    }
}

void GeoZonePlanning::setPlanAudit(const std::optional<GeoZonePlanAudit::Record> &planAudit)
{
    if (planAudit != _planAudit) {
        _planAudit = planAudit;
        emit planAuditChanged();
    }
}

void GeoZonePlanning::setPlanAuditUnread(bool unread)
{
    _planAuditUnread = unread;
    if (unread) {
        emit planNotice(tr("This plan was saved by a newer version of QGroundControl: its record of the geozone data it was checked against "
                           "can't be read by this version, so changes of the geozone data since then aren't reported. "
                           "Saving the plan replaces the record with one of this version."));
    }
}

void GeoZonePlanning::notePlanAuditReplaced()
{
    if (_planAuditUnread) {
        _planAuditUnread = false;
        emit planNotice(tr("The plan was saved with a record of the geozone data of this version of QGroundControl, which replaced the one of the newer version."));
    }
}

void GeoZonePlanning::setMissionArea(const QGeoRectangle &missionArea)
{
    if (missionArea != _missionArea) {
        _missionArea = missionArea;
        emit missionAreaChanged();
    }
}
