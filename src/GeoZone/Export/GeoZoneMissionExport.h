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
#include <QtCore/QPointer>
#include <QtCore/QStringList>

class GeoZoneManager;
class MissionController;

/// Export of the planned mission to a file, like GeoZoneBriefingExport: the mission, the file dialog filters, the
/// folder and name proposed from the open plan, and the error of the last export
class GeoZoneMissionExport : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("MissionController.h")

    Q_PROPERTY(MissionController*   missionController   READ missionController  WRITE setMissionController  NOTIFY missionControllerChanged FINAL)
    Q_PROPERTY(QStringList          nameFilters         READ nameFilters                                    CONSTANT FINAL)
    Q_PROPERTY(QString              errorString         READ errorString                                    NOTIFY errorStringChanged FINAL)

public:
    MissionController *missionController() const;
    void setMissionController(MissionController *missionController);
    /// File dialog filters of the formats, the default first
    QStringList nameFilters() const { return _nameFilters; }
    QString errorString() const { return _errorString; }

    /// @return Folder of the open plan, or the mission folder of the settings
    Q_INVOKABLE QString defaultFolder() const;
    /// @return Name of the open plan with the suffix of the export, or its generic name for new plans
    Q_INVOKABLE QString defaultFileName() const;

    /// Writes the content to the file, replacing it only when complete
    /// @return false on errors, with errorString set
    static bool writeFile(const QString &fileName, const QByteArray &content, QString &errorString);

signals:
    void missionControllerChanged();
    void errorStringChanged();

protected:
    /// @param manager GeoZoneManager::instance() when null
    /// @param suffix Of the file name after the name of the open plan, like "-briefing.pdf"
    /// @param genericName File name for new plans
    GeoZoneMissionExport(GeoZoneManager *manager, const QStringList &nameFilters, const QString &suffix, const QString &genericName, QObject *parent);

    void _setErrorString(const QString &errorString);

    GeoZoneManager *const _manager;

private:
    QPointer<MissionController> _missionController;
    const QStringList _nameFilters;
    const QString _suffix;
    const QString _genericName;
    QString _errorString;
};
