/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/


pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import QGroundControl
import QGroundControl.Controls
import QGroundControl.GeoZone
import QGroundControl.Palette
import QGroundControl.ScreenTools

/// Plan View banner summarising in one line how the mission meets the geozones, centered between leftLimit and
/// rightLimit: the restricted zones it enters by restriction, the zones the returns from along it enter, and whether
/// the geozone data changed since the plan was saved, may be outdated or expired. Below are the primary actions and a Details
/// toggle listing the zones of the mission check by category with the other information. Shown while the mission has
/// items and zones are loaded, or when the data or the plan's authorisations need attention. Exports the geozone
/// briefing of the mission, see exportBriefing(), and its operational intent for a flight authorisation request, see
/// exportOperationalIntent().
Rectangle {
    id:             control
    x:              leftLimit + ((_availableWidth - width) / 2)
    width:          Math.min(Math.max(summaryLabel.implicitWidth, _actionsWidth, detailsButton.checked ? _detailsWidth : 0), _availableWidth - (_margin * 2)) + (_margin * 2)
    height:         column.height + (_margin * 2)
    radius:         ScreenTools.defaultFontPixelWidth / 2
    color:          qgcPal.window
    border.color:   _dataInvalid ? qgcPal.colorRed : (_attention ? qgcPal.warningText : qgcPal.text)
    z:              QGroundControl.zOrderWidgets
    visible:        _checkedMission || check.zones.length > 0 || _outdatedText !== "" || _pendingAuthorisations.length > 0 || check.auditText !== ""

    required property var   missionController
    required property var   map                 ///< Fitted to the crossed zones when the banner is clicked
    property var            geoFenceController: null    ///< Fence expected to exclude the prohibited zones, which can be added to it
    property real           leftLimit:  0
    property real           rightLimit: parent ? parent.width : 0

    readonly property alias missionCheck:   check
    readonly property var   crossedZones:   zones(GeoZoneMissionCheck.Crossed)  ///< GeoZoneInfo of the restricted zones the mission enters

    /// @return GeoZoneInfo of the zones the mission check lists with any of the kinds, see GeoZoneMissionCheck::Kind
    function zones(kind) {
        return check.zones.filter(entry => entry.kind & kind).map(entry => entry.zone)
    }

    // Outdated data, like an old list of temporary restrictions, can miss zones the mission enters. Treated as invalid
    // it is expired, but its zones are still checked: leaving them out would be less safe.
    readonly property bool   _dataInvalid:  GeoZoneManager.repository.dataInvalid
    readonly property string _outdatedText: GeoZoneManager.repository.validityWarning === "" ? "" :
                                                (_dataInvalid ? qsTr("Expired geozone data, its zones are still checked: %1") : qsTr("Geozone data may be outdated: %1"))
                                                    .arg(GeoZoneManager.repository.validityWarning.replace(/\n/g, "; "))

    readonly property var  _pendingAuthorisations: GeoZoneManager.planning.authorisations.pendingAuthorisations
    readonly property real _margin:         ScreenTools.defaultFontPixelWidth
    readonly property real _availableWidth: Math.max(rightLimit - leftLimit, ScreenTools.defaultFontPixelWidth * 20)
    readonly property real _detailsWidth:   ScreenTools.defaultFontPixelWidth * 64
    readonly property bool _checkedMission: missionController.containsItems && GeoZoneManager.zoneCount > 0
    readonly property bool _attention:      crossedZones.length > 0 || _outdatedText !== "" || check.auditText !== ""
    // The actions side by side, which wrap when they don't fit
    readonly property real _actionsWidth:   [ routeButton, fenceButton, exportButton, detailsButton ].filter(button => button.visible)
                                                .reduce((width, button) => width + button.implicitWidth + actionFlow.spacing, -actionFlow.spacing)

    /// @return The entries of the mission check with any of the kinds. A zone loaded from overlapping sources is listed
    ///         once, with the labels of all its entries.
    function _entries(kind) {
        const entries = []
        const byKey = {}
        for (const entry of check.zones) {
            if (!(entry.kind & kind)) {
                continue
            }
            const key = entry.zone.displayName + "\u001F" + entry.zone.restriction
            const existing = byKey[key]
            if (existing) {
                existing.labels = existing.labels.concat(entry.labels.filter(label => !existing.labels.includes(label)))
                continue
            }
            byKey[key] = { zone: entry.zone, kind: entry.kind, labels: entry.labels.slice(), approximationTexts: entry.approximationTexts }
            entries.push(byKey[key])
        }
        return entries
    }

    function _restricted(restriction) {
        return _entries(GeoZoneMissionCheck.Crossed).filter(entry => entry.zone.restriction === restriction)
    }

    /// Names of the entries' zones, with the paths only entering them like "the loiter at item 3"
    function _names(entries) {
        return entries.map(entry => entry.labels.length > 0 && !(entry.kind & GeoZoneMissionCheck.Contingency) ?
                               qsTr("%1 (only %2)").arg(entry.zone.displayName).arg(entry.labels.join(", ")) : entry.zone.displayName).join(", ")
    }

    // One line of counts, the most severe first: [ text, color of a dot before it or "" ]
    readonly property var _summaryParts: {
        const parts = []
        const add = (count, text, color) => {
            if (count > 0) {
                parts.push([ text, color ])
            }
        }
        const addRestricted = (restriction, text) => add(_restricted(restriction).length, text, GeoZoneStyle.restrictionColor(restriction))
        const count = kind => _entries(kind).length
        add(_dataInvalid ? 1 : 0, qsTr("expired data"), qgcPal.colorRed)
        addRestricted(GeoZone.Prohibited, qsTr("%n prohibited", "", _restricted(GeoZone.Prohibited).length))
        addRestricted(GeoZone.ReqAuthorisation, qsTr("%n authorisation required", "", _restricted(GeoZone.ReqAuthorisation).length))
        addRestricted(GeoZone.Conditional, qsTr("%n conditional", "", _restricted(GeoZone.Conditional).length))
        addRestricted(GeoZone.Unknown, qsTr("%n unknown restriction", "", _restricted(GeoZone.Unknown).length))
        add(count(GeoZoneMissionCheck.Unfenced), qsTr("%n not fenced", "", count(GeoZoneMissionCheck.Unfenced)), "")
        add(count(GeoZoneMissionCheck.Contingency), qsTr("returns enter %n zone(s)", "", count(GeoZoneMissionCheck.Contingency)), "")
        add(check.auditText !== "" ? 1 : 0, qsTr("data changed"), "")
        add(_outdatedText !== "" && !_dataInvalid ? 1 : 0, qsTr("data may be outdated"), "")
        add(_pendingAuthorisations.length, qsTr("%n authorisation(s) in the plan", "", _pendingAuthorisations.length), "")
        add(count(GeoZoneMissionCheck.USpace), qsTr("%n U-space", "", count(GeoZoneMissionCheck.USpace)), GeoZoneStyle.restrictionColor(GeoZone.USpace))
        add(count(GeoZoneMissionCheck.Authorised), qsTr("%n authorised", "", count(GeoZoneMissionCheck.Authorised)), "")
        add(count(GeoZoneMissionCheck.Permitted), qsTr("%n without restriction", "", count(GeoZoneMissionCheck.Permitted)), GeoZoneStyle.restrictionColor(GeoZone.NoRestriction))
        add(count(GeoZoneMissionCheck.Inactive), qsTr("%n inactive at the flight time", "", count(GeoZoneMissionCheck.Inactive)), "")
        add(parts.length === 0 && _checkedMission ? 1 : 0, qsTr("No geozones on the mission path"), "")
        return parts
    }

    // Counts only, no texts from the files, with a dot in the color of the restriction
    readonly property string _summaryText: _summaryParts.map(part => part[1] === "" ? part[0] :
                                                                 "<font color=\"%1\">\u25CF</font>&nbsp;%2".arg(part[1]).arg(part[0]))
                                                        .join(" \u00B7 ")

    /// Asks for confirmation before uploading a mission which enters prohibited zones without authorisation, whose
    /// geozone data expired or changed since the plan was saved, listing why, then calls uploadFunction unless cancelled
    function confirmUpload(uploadFunction) {
        // The banner updates after changes settle, so check the current plan
        check.update()
        const reasons = Array.from(check.uploadReasons())
        // Outdated data can miss zones
        if (_outdatedText !== "") {
            reasons.push(_outdatedText)
        }
        if (reasons.length === 0) {
            uploadFunction()
            return
        }
        mainWindow.showMessageDialog(qsTr("Plan Upload"),
                                     qsTr("Confirm the upload of this plan:\n\n%1\n\nClick 'Ok' to upload the Plan anyway.")
                                        .arg(reasons.map(reason => "\u2022 " + control._plainText(reason)).join("\n\n")),
                                     Dialog.Ok | Dialog.Cancel,
                                     uploadFunction)
    }

    /// Asks for a file and exports the geozone briefing of the mission to it, as PDF, web page or JSON by its extension
    function exportBriefing() {
        briefingDialog.folder = briefingExport.defaultFolder()
        briefingDialog.fileName = briefingExport.defaultFileName()
        briefingDialog.openForSave()
    }

    /// Asks for a file and exports the operational intent of the mission to it as JSON, see GeoZoneOperationalIntentExport
    function exportOperationalIntent() {
        intentDialog.folder = intentExport.defaultFolder()
        intentDialog.fileName = intentExport.defaultFileName()
        intentDialog.openForSave()
    }

    /// Markup in names from the files is defused for the message dialog, which detects rich text
    function _plainText(text) {
        return text.replace(/</g, "\u2039").replace(/>/g, "\u203A")
    }

    QGCPalette { id: qgcPal; colorGroupEnabled: true }

    GeoZoneBriefingExport {
        id:                 briefingExport
        missionController:  control.missionController
    }

    QGCFileDialog {
        id:             briefingDialog
        title:          qsTr("Export Geozone Briefing")
        nameFilters:    briefingExport.nameFilters
        defaultSuffix:  "pdf"

        onAcceptedForSave: (file) => {
            close()
            if (briefingExport.exportBriefing(file)) {
                mainWindow.showMessageDialog(qsTr("Export Geozone Briefing"), qsTr("Briefing saved to %1").arg(control._plainText(file)))
            } else {
                mainWindow.showMessageDialog(qsTr("Export Geozone Briefing"), control._plainText(briefingExport.errorString))
            }
        }
    }

    GeoZoneOperationalIntentExport {
        id:                 intentExport
        missionController:  control.missionController
    }

    QGCFileDialog {
        id:             intentDialog
        title:          qsTr("Export Operational Intent")
        nameFilters:    intentExport.nameFilters
        defaultSuffix:  "json"

        onAcceptedForSave: (file) => {
            close()
            if (intentExport.exportIntent(file)) {
                mainWindow.showMessageDialog(qsTr("Export Operational Intent"), qsTr("Operational intent saved to %1").arg(control._plainText(file)))
            } else {
                mainWindow.showMessageDialog(qsTr("Export Operational Intent"), control._plainText(intentExport.errorString))
            }
        }
    }

    GeoZoneMissionCheck {
        id:                 check
        missionController:  control.missionController
        geoFenceController: control.geoFenceController
    }

    Component {
        id: routeDialogComponent

        GeoZoneRouteDialog { }
    }

    GeoZoneFenceImporter {
        id:                 fenceImporter
        geoFenceController: control.geoFenceController
    }

    // A category of the details: a dot in the color of the restriction, a title and the names of the zones
    component Category: RowLayout {
        id:         category
        spacing:    control._margin / 2
        visible:    text !== ""

        property string title
        property string text
        property int    restriction:    -1
        property bool   warning:        false

        Rectangle {
            Layout.alignment:       Qt.AlignTop
            Layout.topMargin:       (ScreenTools.defaultFontPixelHeight - height) / 2
            Layout.preferredWidth:  ScreenTools.defaultFontPixelHeight * 0.6
            Layout.preferredHeight: Layout.preferredWidth
            radius:                 width / 2
            color:                  category.restriction < 0 ? "transparent" : GeoZoneStyle.restrictionColor(category.restriction)
        }

        QGCLabel {
            Layout.fillWidth:   true
            wrapMode:           Text.WordWrap
            textFormat:         Text.PlainText
            color:              category.warning ? qgcPal.warningText : qgcPal.text
            text:               category.title + ": " + category.text
        }
    }

    Column {
        id:                 column
        anchors.centerIn:   parent
        width:              parent.width - (_margin * 2)
        spacing:            control._margin / 2

        QGCLabel {
            id:                     summaryLabel
            width:                  parent.width
            wrapMode:               Text.WordWrap
            horizontalAlignment:    Text.AlignHCenter
            textFormat:             Text.StyledText
            color:                  control._attention ? qgcPal.warningText : qgcPal.text
            text:                   control._summaryText
        }

        Flow {
            id:                         actionFlow
            anchors.horizontalCenter:   parent.horizontalCenter
            width:                      Math.min(control._actionsWidth, parent.width)
            spacing:                    control._margin

            QGCButton {
                id:         routeButton
                text:       qsTr("Route Around Zones...")
                visible:    control.crossedZones.length > 0
                onClicked:  routeDialogComponent.createObject(mainWindow, { missionController: control.missionController }).open()
            }

            QGCButton {
                id:         fenceButton
                text:       qsTr("Add Exclusion Fences")
                visible:    check.fenceText !== ""
                onClicked:  fenceImporter.addZones(control.zones(GeoZoneMissionCheck.Unfenced))
            }

            QGCButton {
                id:         exportButton
                text:       qsTr("Export") + " \u25BE"
                visible:    control._checkedMission
                onClicked:  exportMenu.popup(exportButton, 0, exportButton.height)

                QGCMenu {
                    id: exportMenu

                    QGCMenuItem {
                        text:           qsTr("Geozone Briefing...")
                        onTriggered:    control.exportBriefing()
                    }

                    QGCMenuItem {
                        text:           qsTr("Operational Intent...")
                        onTriggered:    control.exportOperationalIntent()
                    }
                }
            }

            QGCButton {
                id:         detailsButton
                text:       qsTr("Details") + (checked ? " \u25B4" : " \u25BE")
                checkable:  true
                visible:    check.zones.length > 0 || control._outdatedText !== "" || control._pendingAuthorisations.length > 0 || check.auditText !== ""
            }
        }

        // The details scroll when they don't fit in about half the map
        QGCFlickable {
            id:             detailsFlickable
            width:          parent.width
            height:         Math.min(detailsColumn.height, control.map ? control.map.height / 2 : ScreenTools.defaultFontPixelHeight * 20)
            contentHeight:  detailsColumn.height
            clip:           true
            visible:        detailsButton.checked && detailsButton.visible

            Column {
                id:         detailsColumn
                width:      detailsFlickable.width
                spacing:    control._margin / 2

                Category {
                    width:          parent.width
                    title:          qsTr("Prohibited")
                    restriction:    GeoZone.Prohibited
                    warning:        true
                    text:           control._names(control._restricted(GeoZone.Prohibited))
                }

                Category {
                    width:          parent.width
                    title:          qsTr("Authorisation required")
                    restriction:    GeoZone.ReqAuthorisation
                    warning:        true
                    text:           control._names(control._restricted(GeoZone.ReqAuthorisation))
                }

                Category {
                    width:          parent.width
                    title:          qsTr("Conditional")
                    restriction:    GeoZone.Conditional
                    warning:        true
                    text:           control._names(control._restricted(GeoZone.Conditional))
                }

                Category {
                    width:          parent.width
                    title:          qsTr("Unknown restriction")
                    restriction:    GeoZone.Unknown
                    warning:        true
                    text:           control._names(control._restricted(GeoZone.Unknown))
                }

                // Why the check of the zones entered is approximate
                Category {
                    width:  parent.width
                    title:  qsTr("Approximate")
                    text:   control._entries(GeoZoneMissionCheck.Crossed).reduce((texts, entry) => texts.concat(entry.approximationTexts.filter(text => !texts.includes(text))), []).join("; ")
                }

                Category {
                    width:      parent.width
                    title:      qsTr("Not excluded by the GeoFence")
                    warning:    true
                    text:       control._names(control._entries(GeoZoneMissionCheck.Unfenced))
                }

                // One line per zone the returns from along the mission enter, with the returns and the items they start from
                Repeater {
                    model: control._entries(GeoZoneMissionCheck.Contingency)

                    Category {
                        width:          detailsColumn.width
                        title:          qsTr("Returns enter %1").arg(modelData.zone.displayName)
                        restriction:    modelData.zone.restriction
                        text:           modelData.labels.join("; ")

                        required property var modelData
                    }
                }

                Category {
                    width:          parent.width
                    title:          qsTr("U-space")
                    restriction:    GeoZone.USpace
                    text:           control._entries(GeoZoneMissionCheck.USpace).length === 0 ? "" :
                                        qsTr("%1. A flight authorisation from a U-space service provider is required.").arg(control._names(control._entries(GeoZoneMissionCheck.USpace)))
                }

                Category {
                    width:  parent.width
                    title:  qsTr("Authorised")
                    text:   control._names(control._entries(GeoZoneMissionCheck.Authorised))
                }

                Category {
                    width:          parent.width
                    title:          qsTr("Without restriction")
                    restriction:    GeoZone.NoRestriction
                    text:           control._entries(GeoZoneMissionCheck.Permitted).length === 0 ? "" :
                                        qsTr("%1. Check their conditions.").arg(control._names(control._entries(GeoZoneMissionCheck.Permitted)))
                }

                Category {
                    width:  parent.width
                    title:  qsTr("Not active at the flight time")
                    text:   control._names(control._entries(GeoZoneMissionCheck.Inactive))
                }

                QGCLabel {
                    width:      parent.width
                    wrapMode:   Text.WordWrap
                    textFormat: Text.PlainText
                    color:      control._dataInvalid ? qgcPal.colorRed : qgcPal.warningText
                    font.bold:  control._dataInvalid
                    text:       control._outdatedText
                    visible:    text !== ""
                }

                // The zones or sources changed since the plan was saved
                QGCLabel {
                    width:      parent.width
                    wrapMode:   Text.WordWrap
                    textFormat: Text.PlainText
                    color:      qgcPal.warningText
                    text:       check.auditText
                    visible:    text !== ""
                }

                Row {
                    spacing:    control._margin
                    visible:    check.auditText !== ""

                    QGCButton {
                        text:       qsTr("Changes")
                        onClicked:  mainWindow.showMessageDialog(qsTr("Geozone Data Changes"), control._plainText(check.auditDetails.join("\n")))
                    }

                    QGCButton {
                        text:       qsTr("Acknowledge")
                        onClicked:  check.acknowledgeAudit()
                    }
                }

                // Authorisations of an opened plan are the operator's choice to add
                QGCLabel {
                    width:      parent.width
                    wrapMode:   Text.WordWrap
                    textFormat: Text.PlainText
                    text:       qsTr("The plan includes authorisations: %1. Add them to your authorisations?")
                                    .arg(control._pendingAuthorisations.map(authorisation => authorisation.permit !== "" ?
                                            qsTr("%1 (permit %2, %3)").arg(authorisation.zoneName).arg(authorisation.permit).arg(authorisation.statusText) :
                                            qsTr("%1 (%2)").arg(authorisation.zoneName).arg(authorisation.statusText)).join(", "))
                    visible:    control._pendingAuthorisations.length > 0
                }

                Row {
                    spacing:    control._margin
                    visible:    control._pendingAuthorisations.length > 0

                    QGCButton {
                        text:       qsTr("Add")
                        onClicked:  GeoZoneManager.planning.authorisations.acceptPending()
                    }

                    QGCButton {
                        text:       qsTr("Ignore")
                        onClicked:  GeoZoneManager.planning.authorisations.dismissPending()
                    }
                }

                QGCLabel {
                    width:          parent.width
                    wrapMode:       Text.WordWrap
                    font.pointSize: ScreenTools.smallFontPointSize
                    text:           qsTr("Checked for a flight starting %1 (local time).").arg(Qt.formatDateTime(GeoZoneManager.planning.plannedStart, "yyyy-MM-dd HH:mm"))
                    visible:        !isNaN(GeoZoneManager.planning.plannedStart.getTime())
                }

                QGCLabel {
                    width:          parent.width
                    wrapMode:       Text.WordWrap
                    font.pointSize: ScreenTools.smallFontPointSize
                    text:           qsTr("Click the banner to show the restricted zones on the map.")
                    visible:        control.crossedZones.length > 0
                }
            }
        }
    }

    // Below the column, so its buttons get their clicks
    QGCMouseArea {
        anchors.fill:   parent
        z:              -1
        onClicked: {
            GeoZoneManager.display.visible = true
            if (check.crossedZonesBoundingBox.isValid) {
                map.fitViewportToGeoShape(check.crossedZonesBoundingBox, ScreenTools.defaultFontPixelHeight * 2)
            }
        }
    }
}
