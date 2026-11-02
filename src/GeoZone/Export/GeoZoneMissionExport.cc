/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMissionExport.h"
#include "AppSettings.h"
#include "GeoZoneManager.h"
#include "MissionController.h"
#include "PlanMasterController.h"
#include "SettingsManager.h"

#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>

GeoZoneMissionExport::GeoZoneMissionExport(GeoZoneManager *manager, const QStringList &nameFilters, const QString &suffix, const QString &genericName,
                                           QObject *parent)
    : QObject(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
    , _nameFilters(nameFilters)
    , _suffix(suffix)
    , _genericName(genericName)
{
}

MissionController *GeoZoneMissionExport::missionController() const
{
    return _missionController;
}

void GeoZoneMissionExport::setMissionController(MissionController *missionController)
{
    if (missionController != _missionController) {
        _missionController = missionController;
        emit missionControllerChanged();
    }
}

void GeoZoneMissionExport::_setErrorString(const QString &errorString)
{
    if (errorString != _errorString) {
        _errorString = errorString;
        emit errorStringChanged();
    }
}

QString GeoZoneMissionExport::defaultFolder() const
{
    const PlanMasterController *const masterController = _missionController ? _missionController->masterController() : nullptr;
    if (masterController && !masterController->currentPlanFile().isEmpty()) {
        return QFileInfo(masterController->currentPlanFile()).absolutePath();
    }
    return SettingsManager::instance()->appSettings()->missionSavePath();
}

QString GeoZoneMissionExport::defaultFileName() const
{
    const PlanMasterController *const masterController = _missionController ? _missionController->masterController() : nullptr;
    if (masterController && !masterController->currentPlanFile().isEmpty()) {
        return QFileInfo(masterController->currentPlanFile()).completeBaseName() + _suffix;
    }
    return _genericName;
}

bool GeoZoneMissionExport::writeFile(const QString &fileName, const QByteArray &content, QString &errorString)
{
    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly) || (file.write(content) != content.size()) || !file.commit()) {
        errorString = tr("Can't write %1: %2").arg(fileName, file.errorString());
        return false;
    }
    return true;
}
