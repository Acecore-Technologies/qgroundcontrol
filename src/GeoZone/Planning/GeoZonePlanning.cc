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
        _plannedStart = start;
        emit plannedStartChanged();
    }
}

void GeoZonePlanning::setPlanAudit(const std::optional<GeoZonePlanAudit::Record> &planAudit)
{
    if (planAudit != _planAudit) {
        _planAudit = planAudit;
        emit planAuditChanged();
    }
}
