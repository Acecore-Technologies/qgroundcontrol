/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QDate>
#include <QtCore/QString>

/// The AIRAC calendar (ICAO Annex 15): aeronautical data published by AIRAC takes effect on common dates every 28 days.
/// The cycles are computed from the effective date of cycle 2001, 2020-01-02. Cycles are named by the two last digits
/// of the year of their effective date and their number in that year, from 01 (13 or 14 cycles a year), like 2601 on
/// 2026-01-22. Cycles take effect at 00:00 UTC.
namespace GeoZoneAirac {

inline constexpr int cycleDays = 28;

/// @return Effective date of cycle 2001, the epoch of the calendar
QDate epoch();
/// @return Effective date of the cycle in effect on the date, invalid for an invalid date
QDate cycleStart(const QDate &date);
/// @return Effective date of the cycle following the one in effect on the date, when that one ends
QDate nextCycleStart(const QDate &date);
/// @return Name of the cycle in effect on the date, like "2601", empty for an invalid date
QString cycleName(const QDate &date);

} // namespace GeoZoneAirac
