/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QString>

#include <optional>

/// Operator and aircraft of a flight as the Remote ID broadcasts them, which the briefing, the operational intent and the
/// flight record include when the user chooses to, for the authorities and service providers. Filled by the
/// application from its Remote ID settings, texts are empty when not set or undeclared.
struct GeoZoneOperatorDetails
{
    QString operatorId;     ///< Operator registration number
    QString operatorIdType; ///< Like "CAA"
    QString region;         ///< Remote ID rules followed, "FAA" or "EU"
    QString uasId;          ///< Basic ID of the aircraft: serial number, registration or UTM id
    QString uasIdType;      ///< Like "SerialNumber (ANSI/CTA-2063)"
    QString uaType;         ///< Like "Helicopter/Multirotor"
    QString category;       ///< EU category, like "Specific"
    QString aircraftClass;  ///< EU class, like "Class 2"

    /// @return "operator" of the exports: id, idType and region
    QJsonObject operatorJson() const;
    /// @return "aircraft" of the exports: uasId, uasIdType, uaType, category and class
    QJsonObject aircraftJson() const;
    /// @return The "operator" or "aircraft" of the details, null without
    static QJsonValue operatorJson(const std::optional<GeoZoneOperatorDetails> &details);
    static QJsonValue aircraftJson(const std::optional<GeoZoneOperatorDetails> &details);

    friend bool operator==(const GeoZoneOperatorDetails &, const GeoZoneOperatorDetails &) = default;
};
