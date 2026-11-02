/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneOperatorDetails.h"

namespace {

QJsonValue text(const QString &value)
{
    return value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value);
}

} // namespace

QJsonObject GeoZoneOperatorDetails::operatorJson() const
{
    return QJsonObject{
        { QStringLiteral("id"), text(operatorId) },
        { QStringLiteral("idType"), text(operatorIdType) },
        { QStringLiteral("region"), text(region) },
    };
}

QJsonObject GeoZoneOperatorDetails::aircraftJson() const
{
    return QJsonObject{
        { QStringLiteral("uasId"), text(uasId) },
        { QStringLiteral("uasIdType"), text(uasIdType) },
        { QStringLiteral("uaType"), text(uaType) },
        { QStringLiteral("category"), text(category) },
        { QStringLiteral("class"), text(aircraftClass) },
    };
}

QJsonValue GeoZoneOperatorDetails::operatorJson(const std::optional<GeoZoneOperatorDetails> &details)
{
    return details ? QJsonValue(details->operatorJson()) : QJsonValue(QJsonValue::Null);
}

QJsonValue GeoZoneOperatorDetails::aircraftJson(const std::optional<GeoZoneOperatorDetails> &details)
{
    return details ? QJsonValue(details->aircraftJson()) : QJsonValue(QJsonValue::Null);
}
