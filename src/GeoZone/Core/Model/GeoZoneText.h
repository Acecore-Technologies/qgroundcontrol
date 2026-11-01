/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QString>

#include "GeoZone.h"

/// Translated display texts of geozones, with altitudes in the vertical distance units of the application, see
/// GeoZoneEnvironment. The zone model has no translated texts or units of its own, its text accessors, like
/// GeoZone::reasonText, are made here.
class GeoZoneText
{
    Q_DECLARE_TR_FUNCTIONS(GeoZoneText)

public:
    static QString restriction(GeoZone::Restriction restriction);

    /// @return The published reasons, named when known, with the other reason information
    static QString reasons(const GeoZone::Data &data);

    /// @return Whether the regulation allows exemptions, empty when not published
    static QString regulationExemption(const GeoZone::Data &data);

    /// @return Limits of each volume on a line. Flight levels show their approximate height above mean sea level with
    ///         the QNH.
    static QString verticalLimits(const GeoZone::Data &data);

    /// @return A limit as published, converted to the user's units with the published value when they differ
    static QString limit(double limit, GeoZone::VerticalReference reference, const QString &referenceCode, bool feet, double qnhHPa);
};
