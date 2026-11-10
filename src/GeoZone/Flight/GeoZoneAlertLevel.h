/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

/// Levels of the flight alerts after EN 4709-003 Annex D, shared by all alerts the pilot is shown: the geozone alerts,
/// the vehicle's fence and the state of the geo-awareness. The popup, the indicator and the map show them in their
/// colors (GeoZoneStyle.levelColor), the alert log records them and the speech starts with their word.
namespace GeoZoneAlertLevel {
Q_NAMESPACE

enum Level {
    Warning = 0,    ///< Red, immediate action: inside a restricted zone without approval, its entry imminent, or it became active around the vehicle
    Caution = 1,    ///< Amber, awareness and action soon: approaching a restricted zone, an activation upcoming, degraded geo-awareness
    Advisory = 2,   ///< Neutral, information: conditional zones, approved zones, traffic
};
Q_ENUM_NS(Level)

constexpr int severityStride = 10;  ///< Severities within a level, more than those of GeoZone::restrictionSeverity

/// @return Severity of an alert, lower is more urgent: by level, then by the zone's GeoZone::restrictionSeverity
constexpr int severity(Level level, int restrictionSeverity) { return (static_cast<int>(level) * severityStride) + restrictionSeverity; }

/// @return "WARNING", "CAUTION" or "ADVISORY", as the popup shows it
QString text(Level level);
/// @return "warning", "caution" or "advisory", as the alert log records it
QString code(Level level);
/// @return Level of a code, Caution for unknown codes
Level fromCode(const QString &code, bool *ok = nullptr);
/// @return Word starting the speech of an alert: "Warning" or "Caution", none for advisories
QString spokenPrefix(Level level);

} // namespace GeoZoneAlertLevel
