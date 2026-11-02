/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneEd318Writer.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>

#include <algorithm>
#include <cmath>

namespace {

QString _referenceCode(const QString &published, GeoZone::VerticalReference reference)
{
    if (!published.isEmpty()) {
        return published;
    }
    switch (reference) {
    case GeoZone::VerticalReference::Ground:
        return QStringLiteral("AGL");
    case GeoZone::VerticalReference::MeanSeaLevel:
        return QStringLiteral("AMSL");
    case GeoZone::VerticalReference::Ellipsoid:
        return QStringLiteral("WGS84");
    case GeoZone::VerticalReference::FlightLevel:
        return QStringLiteral("STD");
    case GeoZone::VerticalReference::Unknown:
        break;
    }
    return QString();
}

QJsonArray _position(const QPointF &vertex)
{
    return QJsonArray{ vertex.x(), vertex.y() };
}

/// Twice the signed area of the ring, positive when counterclockwise
double _signedArea(const GeoZone::Ring &ring)
{
    double area = 0;
    for (qsizetype i = 0; i < ring.count(); i++) {
        const QPointF &a = ring[i];
        const QPointF &b = ring[(i + 1) % ring.count()];
        area += (a.x() * b.y()) - (b.x() * a.y());
    }
    return area;
}

/// GeoJSON ring, closed, counterclockwise for the outer ring and clockwise for holes (RFC 7946 section 3.1.6)
QJsonArray _ring(GeoZone::Ring ring, bool outer)
{
    if (!ring.isEmpty() && (ring.first() == ring.last())) {
        ring.removeLast();
    }
    if ((_signedArea(ring) > 0) != outer) {
        std::reverse(ring.begin(), ring.end());
    }
    QJsonArray positions;
    for (const QPointF &vertex : std::as_const(ring)) {
        positions.append(_position(vertex));
    }
    if (!ring.isEmpty()) {
        positions.append(_position(ring.first()));
    }
    return positions;
}

QJsonObject _geometry(const GeoZone::Volume &volume)
{
    QJsonObject geometry;
    if (volume.isCircle) {
        geometry.insert(QStringLiteral("type"), QStringLiteral("Point"));
        geometry.insert(QStringLiteral("coordinates"), QJsonArray{ volume.center.longitude(), volume.center.latitude() });
        geometry.insert(QStringLiteral("extent"), QJsonObject{ { QStringLiteral("subType"), QStringLiteral("Circle") }, { QStringLiteral("radius"), volume.radius } });
    } else {
        QJsonArray rings;
        for (qsizetype i = 0; i < volume.rings.count(); i++) {
            rings.append(_ring(volume.rings[i], i == 0));
        }
        geometry.insert(QStringLiteral("type"), QStringLiteral("Polygon"));
        geometry.insert(QStringLiteral("coordinates"), rings);
    }

    // Volumes without limits apply at all altitudes, which a missing layer means
    if (!std::isnan(volume.lowerLimit) || !std::isnan(volume.upperLimit)) {
        QJsonObject layer{ { QStringLiteral("uom"), volume.feet ? QStringLiteral("ft") : QStringLiteral("m") } };
        if (!std::isnan(volume.lowerLimit)) {
            layer.insert(QStringLiteral("lower"), volume.lowerLimit);
        }
        if (!std::isnan(volume.upperLimit)) {
            layer.insert(QStringLiteral("upper"), volume.upperLimit);
        }
        layer.insert(QStringLiteral("lowerReference"), _referenceCode(volume.lowerReferenceCode, volume.lowerReference));
        layer.insert(QStringLiteral("upperReference"), _referenceCode(volume.upperReferenceCode, volume.upperReference));
        geometry.insert(QStringLiteral("layer"), layer);
    }
    return geometry;
}

} // namespace

namespace GeoZoneEd318Writer {

QString restrictionCode(GeoZone::Restriction restriction)
{
    switch (restriction) {
    case GeoZone::Prohibited:
        return QStringLiteral("PROHIBITED");
    case GeoZone::ReqAuthorisation:
        return QStringLiteral("REQ_AUTHORIZATION");
    case GeoZone::Conditional:
        return QStringLiteral("CONDITIONAL");
    case GeoZone::NoRestriction:
        return QStringLiteral("NO_RESTRICTION");
    case GeoZone::USpace:
        return QStringLiteral("USPACE");
    case GeoZone::Unknown:
        break;
    }
    return QString();
}

QJsonObject feature(const GeoZone::Data &zone)
{
    QJsonObject properties;
    const auto insertText = [&properties](const char *name, const QString &text) {
        if (!text.isEmpty()) {
            properties.insert(QLatin1String(name), text);
        }
    };
    insertText("identifier", zone.identifier);
    insertText("country", zone.country);
    insertText("name", zone.name);
    insertText("type", zone.restrictionCode.isEmpty() ? restrictionCode(zone.restriction) : zone.restrictionCode);
    if (!zone.reasons.isEmpty()) {
        properties.insert(QStringLiteral("reason"), QJsonArray::fromStringList(zone.reasons));
    }
    insertText("otherReasonInfo", zone.otherReasonInfo);
    insertText("message", zone.message);
    insertText("restrictionConditions", zone.restrictionConditions);
    if (!zone.authorities.isEmpty()) {
        QJsonArray authorities;
        for (const QString &authority : zone.authorities) {
            authorities.append(QJsonObject{ { QStringLiteral("name"), authority } });
        }
        properties.insert(QStringLiteral("zoneAuthority"), authorities);
    }

    QJsonObject geometry;
    if (zone.volumes.count() == 1) {
        geometry = _geometry(zone.volumes.first());
    } else {
        QJsonArray geometries;
        for (const GeoZone::Volume &volume : zone.volumes) {
            geometries.append(_geometry(volume));
        }
        geometry = QJsonObject{ { QStringLiteral("type"), QStringLiteral("GeometryCollection") }, { QStringLiteral("geometries"), geometries } };
    }

    QJsonObject feature{ { QStringLiteral("type"), QStringLiteral("Feature") }, { QStringLiteral("properties"), properties }, { QStringLiteral("geometry"), geometry } };
    if (!zone.identifier.isEmpty()) {
        feature.insert(QStringLiteral("id"), zone.identifier);
    }
    return feature;
}

QByteArray document(const QList<GeoZone::Data> &zones, const QString &title, const QString &provider, const QDateTime &issued)
{
    QJsonArray features;
    for (const GeoZone::Data &zone : zones) {
        features.append(feature(zone));
    }
    QJsonObject metadata;
    if (!provider.isEmpty()) {
        metadata.insert(QStringLiteral("provider"), provider);
    }
    if (issued.isValid()) {
        metadata.insert(QStringLiteral("issued"), issued.toUTC().toString(Qt::ISODate));
    }
    QJsonObject root{ { QStringLiteral("type"), QStringLiteral("FeatureCollection") }, { QStringLiteral("features"), features } };
    if (!title.isEmpty()) {
        root.insert(QStringLiteral("name"), title);
    }
    if (!metadata.isEmpty()) {
        root.insert(QStringLiteral("metadata"), metadata);
    }
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

} // namespace GeoZoneEd318Writer
