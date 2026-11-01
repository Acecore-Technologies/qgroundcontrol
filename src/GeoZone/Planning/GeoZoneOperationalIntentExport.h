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

#include <optional>

#include "GeoZoneOperationalIntent.h"

class GeoZoneManager;
class MissionController;

/// Exports the operational intent of the planned mission, see GeoZoneOperationalIntent, as JSON for a U-space or UTM
/// flight authorisation request: the 4D volumes of the flight from its planned start in the ASTM F3548-21 data model,
/// with the buffers of the settings, the U-space airspaces it enters and the authorisations held for the zones it
/// enters. It isn't submitted to a service provider. See docs/en/qgc-user-guide/geozones/geozones.md for the schema.
class GeoZoneOperationalIntentExport : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("MissionController.h")

    Q_PROPERTY(MissionController*   missionController   READ missionController  WRITE setMissionController  NOTIFY missionControllerChanged FINAL)
    Q_PROPERTY(QStringList          nameFilters         READ nameFilters                                    CONSTANT FINAL)
    Q_PROPERTY(QString              errorString         READ errorString                                    NOTIFY errorStringChanged FINAL)

public:
    explicit GeoZoneOperationalIntentExport(QObject *parent = nullptr);
    /// @param manager Zones and planned start of the flight, GeoZoneManager::instance() when null
    explicit GeoZoneOperationalIntentExport(GeoZoneManager *manager, QObject *parent = nullptr);

    MissionController *missionController() const;
    void setMissionController(MissionController *missionController);
    static QStringList nameFilters();
    QString errorString() const { return _errorString; }

    /// @return Folder of the open plan, or the mission folder of the settings
    Q_INVOKABLE QString defaultFolder() const;
    /// @return Name of the open plan with "-intent.json", or a generic name for new plans
    Q_INVOKABLE QString defaultFileName() const;
    /// Writes the operational intent of the current mission as JSON, adding the extension when missing
    /// @return false on errors, like without a planned start, see errorString
    Q_INVOKABLE bool exportIntent(const QString &fileName);

    /// @return The operational intent of the current mission from the planned start, nothing on errors with
    ///         errorString set
    std::optional<GeoZoneOperationalIntent> intent(QString &errorString) const;
    /// @return The buffers and the maximum volume count of the settings
    static GeoZoneOperationalIntent::Parameters parameters();

signals:
    void missionControllerChanged();
    void errorStringChanged();

private:
    void _setErrorString(const QString &errorString);

    GeoZoneManager *_manager = nullptr;
    QPointer<MissionController> _missionController;
    QString _errorString;
};
