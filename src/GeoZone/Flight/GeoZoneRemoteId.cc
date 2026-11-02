/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneRemoteId.h"
#include "Fact.h"
#include "GeoZoneSettings.h"
#include "RemoteIDManager.h"
#include "RemoteIDSettings.h"
#include "SettingsManager.h"
#include "Vehicle.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>

namespace GeoZoneRemoteId {

namespace {

/// Translation context of the texts
class Text
{
    Q_DECLARE_TR_FUNCTIONS(GeoZoneRemoteId)
};

} // namespace

Status status(Vehicle *vehicle)
{
    Status result;
    RemoteIDSettings *const settings = SettingsManager::instance()->remoteIDSettings();
    const bool operatorIdSet = !settings->operatorID()->rawValue().toString().trimmed().isEmpty();
    // EU operator IDs have a checksum, which the settings page verifies
    const bool eu = settings->region()->rawValue().toInt() == RemoteIDManager::EU;
    result.operatorIdGood = operatorIdSet && (!eu || settings->operatorIDValid()->rawValue().toBool());

    if (!vehicle || vehicle->isOfflineEditingVehicle()) {
        return result;
    }
    result.vehicle = true;
    if (const RemoteIDManager *const manager = vehicle->remoteIDManager()) {
        result.available = manager->available();
        result.commsGood = manager->commsGood();
        result.armStatusGood = manager->armStatusGood();
        result.armStatusError = manager->armStatusError().trimmed();
        result.operatorIdGood = result.operatorIdGood || manager->operatorIDGood();
    }
    return result;
}

QString problem(const Status &status)
{
    QStringList problems;
    if (!status.operatorIdGood) {
        problems.append(Text::tr("The operator ID isn't set or isn't valid (Application Settings > Remote ID)."));
    }
    if (status.vehicle) {
        if (!status.available) {
            problems.append(Text::tr("The vehicle reports no Remote ID."));
        } else if (!status.commsGood) {
            problems.append(Text::tr("The Remote ID stopped reporting its status."));
        } else if (!status.armStatusGood) {
            problems.append(status.armStatusError.isEmpty() ? Text::tr("The Remote ID isn't ready to arm.") :
                                                              Text::tr("The Remote ID isn't ready to arm: %1.").arg(status.armStatusError));
        }
    }
    return problems.join(QLatin1Char(' '));
}

GeoZoneOperatorDetails operatorDetails()
{
    RemoteIDSettings *const settings = SettingsManager::instance()->remoteIDSettings();
    const auto text = [](Fact *fact) { return fact->rawValue().toString().trimmed(); };
    // Enumerated settings by their text, empty when undeclared
    const auto enumText = [](Fact *fact) { return (fact->rawValue().toInt() == 0) ? QString() : fact->enumStringValue(); };
    GeoZoneOperatorDetails details;
    details.operatorId = text(settings->operatorID());
    details.operatorIdType = details.operatorId.isEmpty() ? QString() : settings->operatorIDType()->enumStringValue();
    details.region = settings->region()->enumStringValue();
    details.uasId = text(settings->basicID());
    details.uasIdType = details.uasId.isEmpty() ? QString() : enumText(settings->basicIDType());
    details.uaType = enumText(settings->basicIDUaType());
    // The category and class are declared with the EU classification only
    if (settings->classificationType()->rawValue().toInt() != 0) {
        details.category = enumText(settings->categoryEU());
        details.aircraftClass = enumText(settings->classEU());
    }
    return details;
}

std::optional<GeoZoneOperatorDetails> exportedOperatorDetails()
{
    if (!GeoZoneSettings::instance()->includeOperatorDetails()->rawValue().toBool()) {
        return std::nullopt;
    }
    return operatorDetails();
}

} // namespace GeoZoneRemoteId
