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

/// Which geozones the maps draw: all of them or none, and the restrictions the legend hides. The mission check,
/// flight alerts and guided command checks apply to all zones. Kept in GeoZoneSettings.
class GeoZoneDisplay : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool visible             READ visible            WRITE setVisible    NOTIFY visibleChanged FINAL)
    Q_PROPERTY(int  hiddenRestrictions  READ hiddenRestrictions                     NOTIFY hiddenRestrictionsChanged FINAL)   ///< Bit mask of the restrictions not drawn on maps, 1 << GeoZone::Restriction

public:
    explicit GeoZoneDisplay(QObject *parent = nullptr);

    bool visible() const;
    void setVisible(bool visible);
    int hiddenRestrictions() const;
    /// @return true if zones with the restriction are drawn on maps
    Q_INVOKABLE bool restrictionVisible(int restriction) const { return (hiddenRestrictions() & (1 << restriction)) == 0; }
    /// Shows or hides the zones with the restriction on maps
    Q_INVOKABLE void setRestrictionVisible(int restriction, bool visible);

signals:
    void visibleChanged(bool visible);
    void hiddenRestrictionsChanged();
};
