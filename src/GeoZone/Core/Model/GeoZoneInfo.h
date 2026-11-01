/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include "GeoZone.h"
#include "GeoZoneText.h"

#include <QtCore/QMetaType>
#include <QtCore/QSharedPointer>
#include <QtCore/QVariantList>

/// A zone as seen by QML: a value sharing the zone, which stays valid when its source is reloaded or removed.
/// Properties are read from the zone when used.
class GeoZoneInfo
{
    Q_GADGET

    Q_PROPERTY(bool             valid                   READ valid                  CONSTANT FINAL)
    Q_PROPERTY(QString          key                     READ key                    CONSTANT FINAL)   ///< See GeoZone::key
    Q_PROPERTY(QString          identifier              READ identifier             CONSTANT FINAL)
    Q_PROPERTY(QString          name                    READ name                   CONSTANT FINAL)
    Q_PROPERTY(QString          displayName             READ displayName            CONSTANT FINAL)   ///< Name, or the identifier of unnamed zones
    Q_PROPERTY(QString          country                 READ country                CONSTANT FINAL)
    Q_PROPERTY(int              restriction             READ restriction            CONSTANT FINAL)   ///< GeoZone::Restriction
    Q_PROPERTY(QString          restrictionText         READ restrictionText        CONSTANT FINAL)
    Q_PROPERTY(QString          restrictionConditions   READ restrictionConditions  CONSTANT FINAL)
    Q_PROPERTY(QString          reasonText              READ reasonText             CONSTANT FINAL)
    Q_PROPERTY(QString          message                 READ message                CONSTANT FINAL)
    Q_PROPERTY(QString          authorityText           READ authorityText          CONSTANT FINAL)
    Q_PROPERTY(QString          uSpaceClass             READ uSpaceClass            CONSTANT FINAL)
    Q_PROPERTY(QString          regulationExemptionText READ regulationExemptionText CONSTANT FINAL)
    Q_PROPERTY(QString          extendedPropertiesText  READ extendedPropertiesText CONSTANT FINAL)  ///< Publisher specific properties, one per line
    Q_PROPERTY(QString          issuesText              READ issuesText             CONSTANT FINAL)  ///< Problems reading the zone, one per line
    Q_PROPERTY(QString          applicabilityText       READ applicabilityText      CONSTANT FINAL)
    Q_PROPERTY(QString          verticalLimitsText      READ verticalLimitsText     CONSTANT FINAL)
    Q_PROPERTY(bool             approximateLimits       READ approximateLimits      CONSTANT FINAL)   ///< Flight levels converted with the QNH
    Q_PROPERTY(QGeoCoordinate   center                  READ center                 CONSTANT FINAL)
    Q_PROPERTY(QGeoRectangle    boundingBox             READ boundingBox            CONSTANT FINAL)
    Q_PROPERTY(bool             active                  READ active                 CONSTANT FINAL)   ///< When read
    Q_PROPERTY(QString          sourceKey               READ sourceKey              CONSTANT FINAL)   ///< Key of the GeoZoneSource
    Q_PROPERTY(QString          sourceText              READ sourceText             CONSTANT FINAL)   ///< File or feed the zone was loaded from, with its date

public:
    GeoZoneInfo() = default;
    explicit GeoZoneInfo(const GeoZone *zone);

    const GeoZone *zone() const { return _zone.data(); }

    bool valid() const { return !_zone.isNull(); }
    QString key() const { return _zone ? _zone->key() : QString(); }
    QString identifier() const { return _zone ? _zone->identifier() : QString(); }
    QString name() const { return _zone ? _zone->name() : QString(); }
    QString displayName() const { return _zone ? _zone->displayName() : QString(); }
    QString country() const { return _zone ? _zone->country() : QString(); }
    int restriction() const { return _zone ? static_cast<int>(_zone->restriction()) : static_cast<int>(GeoZone::Unknown); }
    QString restrictionText() const { return _zone ? GeoZoneText::restriction(_zone->restriction()) : QString(); }
    QString restrictionConditions() const { return _zone ? _zone->restrictionConditions() : QString(); }
    QString reasonText() const { return _zone ? GeoZoneText::reasons(_zone->data()) : QString(); }
    QString message() const { return _zone ? _zone->message() : QString(); }
    QString authorityText() const { return _zone ? _zone->authorityText() : QString(); }
    QString uSpaceClass() const { return _zone ? _zone->uSpaceClass() : QString(); }
    QString regulationExemptionText() const { return _zone ? GeoZoneText::regulationExemption(_zone->data()) : QString(); }
    QString extendedPropertiesText() const { return _zone ? _zone->extendedPropertiesText() : QString(); }
    QString issuesText() const { return _zone ? _zone->issuesText() : QString(); }
    QString applicabilityText() const { return _zone ? _zone->applicabilityText() : QString(); }
    QString verticalLimitsText() const { return _zone ? GeoZoneText::verticalLimits(_zone->data()) : QString(); }
    bool approximateLimits() const { return _zone && _zone->approximateLimits(); }
    QGeoCoordinate center() const { return _zone ? _zone->center() : QGeoCoordinate(); }
    QGeoRectangle boundingBox() const { return _zone ? _zone->boundingBox() : QGeoRectangle(); }
    bool active() const { return _zone && _zone->active(); }
    QString sourceKey() const { return _zone ? _zone->sourceKey() : QString(); }
    QString sourceText() const { return _zone ? _zone->sourceText() : QString(); }

    friend bool operator==(const GeoZoneInfo &a, const GeoZoneInfo &b) { return a._zone == b._zone; }

    /// @return Values of the zones for QML
    template<typename Zone>
    static QVariantList toVariantList(const QList<Zone*> &zones)
    {
        QVariantList list;
        list.reserve(zones.count());
        for (const GeoZone *zone : zones) {
            list.append(QVariant::fromValue(GeoZoneInfo(zone)));
        }
        return list;
    }

    /// @return The zone of a value passed from QML, null for other values
    static const GeoZone *fromVariant(const QVariant &value);
    /// @return The zones of the values passed from QML, skipping other values
    static QList<const GeoZone*> fromVariantList(const QVariantList &values);

private:
    QSharedPointer<const GeoZone> _zone;
};

Q_DECLARE_METATYPE(GeoZoneInfo)
