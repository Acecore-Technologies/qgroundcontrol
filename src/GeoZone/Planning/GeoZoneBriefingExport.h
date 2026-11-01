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
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QStringList>

#include <optional>

#include "GeoZoneBriefing.h"

class GeoZoneManager;
class MissionController;

/// Exports the pre-flight geozone briefing of the planned mission, see GeoZoneBriefing, as JSON for other tools, as a
/// self-contained web page readable on phones, or as an A4 PDF document.
///
/// The JSON has a stable schema, see docs/en/qgc-user-guide/geozones/geozones.md: "schema" is
/// "qgc-geozone-briefing" and "version" jsonVersion, which increases when fields change meaning or are removed, not
/// when fields are added. Times are ISO 8601 UTC, distances and altitudes meters, unknown values null.
class GeoZoneBriefingExport : public QObject
{
    Q_OBJECT
    Q_MOC_INCLUDE("MissionController.h")

    Q_PROPERTY(MissionController*   missionController   READ missionController  WRITE setMissionController  NOTIFY missionControllerChanged FINAL)
    Q_PROPERTY(QStringList          nameFilters         READ nameFilters                                    CONSTANT FINAL)
    Q_PROPERTY(QString              errorString         READ errorString                                    NOTIFY errorStringChanged FINAL)

public:
    explicit GeoZoneBriefingExport(QObject *parent = nullptr);
    /// @param manager Zones to brief, GeoZoneManager::instance() when null
    explicit GeoZoneBriefingExport(GeoZoneManager *manager, QObject *parent = nullptr);

    enum class Format {
        Json,
        Html,
        Pdf,
    };

    static constexpr const char *jsonSchema = "qgc-geozone-briefing";
    static constexpr int jsonVersion = 1;

    MissionController *missionController() const;
    void setMissionController(MissionController *missionController);
    /// File dialog filters of the formats, PDF first
    static QStringList nameFilters();
    QString errorString() const { return _errorString; }

    /// @return Folder of the open plan, or the mission folder of the settings
    Q_INVOKABLE QString defaultFolder() const;
    /// @return Name of the open plan with "-briefing.pdf", or a generic name for new plans
    Q_INVOKABLE QString defaultFileName() const;
    /// Writes the briefing of the current mission in the format of the file extension, as PDF without one
    /// @return false on errors, see errorString
    Q_INVOKABLE bool exportBriefing(const QString &fileName);

    /// @return Briefing of the current mission with the zones of the manager
    GeoZoneBriefing briefing() const;

    /// @return Format of the file extension, nothing for other extensions
    static std::optional<Format> formatOf(const QString &fileName);
    /// @return The briefing in the JSON schema, see the class description
    static QJsonObject toJson(const GeoZoneBriefing &briefing);
    /// @return A self-contained HTML document, or with styles QTextDocument supports forPrint
    static QString toHtml(const GeoZoneBriefing &briefing, bool forPrint = false);
    /// Writes the briefing to an A4 PDF document, with its title, date and page numbers on each page
    static bool writePdf(const GeoZoneBriefing &briefing, const QString &fileName, QString &errorString);
    /// Writes the briefing in the format
    static bool write(const GeoZoneBriefing &briefing, const QString &fileName, Format format, QString &errorString);

signals:
    void missionControllerChanged();
    void errorStringChanged();

private:
    void _setErrorString(const QString &errorString);

    GeoZoneManager *_manager = nullptr;
    QPointer<MissionController> _missionController;
    QString _errorString;
};
