/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <optional>

#include "GeoZoneMissionExport.h"
#include "GeoZoneOperationalIntent.h"

/// Exports the operational intent of the planned mission, see GeoZoneOperationalIntent, as JSON for a U-space or UTM
/// flight authorisation request: the 4D volumes of the flight from its planned start in the ASTM F3548-21 data model,
/// with the default buffers of GeoZoneOperationalIntent::Parameters, the U-space airspaces it enters and the authorisations held for the zones it
/// enters. It isn't submitted to a service provider. See docs/en/qgc-user-guide/geozones/geozones.md for the schema.
class GeoZoneOperationalIntentExport : public GeoZoneMissionExport
{
    Q_OBJECT

public:
    /// @param manager Zones and planned start of the flight, GeoZoneManager::instance() when null
    explicit GeoZoneOperationalIntentExport(GeoZoneManager *manager = nullptr, QObject *parent = nullptr);

    /// Writes the operational intent of the current mission as JSON, adding the extension when missing
    /// @return false on errors, like without a planned start, see errorString
    Q_INVOKABLE bool exportIntent(const QString &fileName);

    /// @return The operational intent of the current mission from the planned start, nothing on errors with
    ///         errorString set
    std::optional<GeoZoneOperationalIntent> intent(QString &errorString) const;
};
