/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneText.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneVertical.h"

#include <QtCore/QHash>
#include <QtCore/QStringList>

#include <cmath>

QString GeoZoneText::restriction(GeoZone::Restriction restriction)
{
    switch (restriction) {
    case GeoZone::Prohibited:
        return tr("Prohibited");
    case GeoZone::ReqAuthorisation:
        return tr("Authorisation required");
    case GeoZone::Conditional:
        return tr("Conditional");
    case GeoZone::NoRestriction:
        return tr("No restriction");
    case GeoZone::USpace:
        return tr("U-space airspace");
    case GeoZone::Unknown:
        break;
    }
    return tr("Unknown");
}

QString GeoZoneText::reasons(const GeoZone::Data &data)
{
    static const QHash<QString, const char*> reasonNames = {
        { QStringLiteral("AIR_TRAFFIC"),        QT_TR_NOOP("Air traffic") },
        { QStringLiteral("SENSITIVE"),          QT_TR_NOOP("Sensitive area") },
        { QStringLiteral("PRIVACY"),            QT_TR_NOOP("Privacy") },
        { QStringLiteral("POPULATION"),         QT_TR_NOOP("Population") },
        { QStringLiteral("NATURE"),             QT_TR_NOOP("Nature") },
        { QStringLiteral("NOISE"),              QT_TR_NOOP("Noise") },
        { QStringLiteral("FOREIGN_TERRITORY"),  QT_TR_NOOP("Foreign territory") },
        { QStringLiteral("EMERGENCY"),          QT_TR_NOOP("Emergency") },
        { QStringLiteral("OTHER"),              QT_TR_NOOP("Other") },
    };

    QStringList names;
    for (const QString &reason : data.reasons) {
        const auto it = reasonNames.constFind(reason.toUpper());
        names.append((it != reasonNames.constEnd()) ? tr(it.value()) : reason);
    }

    QString text = names.join(QStringLiteral(", "));
    if (!data.otherReasonInfo.isEmpty()) {
        text = text.isEmpty() ? data.otherReasonInfo : QStringLiteral("%1 (%2)").arg(text, data.otherReasonInfo);
    }
    return text;
}

QString GeoZoneText::regulationExemption(const GeoZone::Data &data)
{
    const QString code = data.regulationExemption.trimmed().toUpper();
    if (code == QStringLiteral("YES")) {
        return tr("Exemptions possible");
    }
    if (code == QStringLiteral("NO")) {
        return tr("No exemptions");
    }
    return data.regulationExemption;
}

QString GeoZoneText::verticalLimits(const GeoZone::Data &data)
{
    const double qnh = GeoZoneEnvironment::qnhHPa();
    const auto convertedFlightLevel = [](double value, GeoZone::VerticalReference reference) {
        return (reference == GeoZone::VerticalReference::FlightLevel) && !qIsNaN(value) && (value < GeoZoneVertical::unlimitedSentinel);
    };
    QStringList lines;
    for (const GeoZone::Volume &volume : data.volumes) {
        QString line;
        if (qIsNaN(volume.lowerLimit) && qIsNaN(volume.upperLimit)) {
            line = tr("Not specified");
        } else {
            line = QStringLiteral("%1 - %2").arg(limit(volume.lowerLimit, volume.lowerReference, volume.lowerReferenceCode, volume.feet, qnh),
                                                 limit(volume.upperLimit, volume.upperReference, volume.upperReferenceCode, volume.feet, qnh));
            if (convertedFlightLevel(volume.lowerLimit, volume.lowerReference) || convertedFlightLevel(volume.upperLimit, volume.upperReference)) {
                line += QStringLiteral(" ") + tr("(QNH %1 hPa)").arg(QString::number(qnh, 'g', 6));
            }
        }
        if (!lines.contains(line)) {
            lines.append(line);
        }
    }
    return lines.join(QStringLiteral("\n"));
}

QString GeoZoneText::limit(double limit, GeoZone::VerticalReference reference, const QString &referenceCode, bool feet, double qnhHPa)
{
    if (qIsNaN(limit)) {
        return tr("not specified");
    }
    if (limit >= GeoZoneVertical::unlimitedSentinel) {
        return tr("Unlimited");
    }
    if ((limit == 0) && (reference == GeoZone::VerticalReference::Ground)) {
        return tr("GND");
    }
    if (reference == GeoZone::VerticalReference::FlightLevel) {
        // In hundreds of feet of pressure altitude, with the height it is at with the QNH in the standard atmosphere
        const double pressureAltitude = feet ? (limit * GeoZoneVertical::metersPerFoot) : limit;
        const double flightLevel = std::round(pressureAltitude / GeoZoneVertical::metersPerFoot / 10) / 10;
        const double amsl = GeoZoneEnvironment::metersToVerticalDistanceUnits(GeoZoneVertical::pressureAltitudeToAMSL(pressureAltitude, qnhHPa));
        return tr("FL %1 ≈ %2 %3 AMSL").arg(flightLevel).arg(qRound(amsl)).arg(GeoZoneEnvironment::verticalDistanceUnits());
    }

    const QString publishedUnits = feet ? QStringLiteral("ft") : QStringLiteral("m");
    const QString userUnits = GeoZoneEnvironment::verticalDistanceUnits();
    if (userUnits == publishedUnits) {
        return QStringLiteral("%1 %2 %3").arg(limit).arg(publishedUnits, referenceCode);
    }

    // Show the published value too since the converted value is rounded
    const double meters = feet ? (limit * GeoZoneVertical::metersPerFoot) : limit;
    const double converted = GeoZoneEnvironment::metersToVerticalDistanceUnits(meters);
    return QStringLiteral("%1 %2 %3 (%4 %5)").arg(qRound(converted)).arg(userUnits, referenceCode).arg(limit).arg(publishedUnits);
}

// The text accessors of the zones, so that the model doesn't depend on translations and units

QString GeoZone::restrictionToString(Restriction restriction)
{
    return GeoZoneText::restriction(restriction);
}

QString GeoZone::reasonText() const
{
    return GeoZoneText::reasons(_data);
}

QString GeoZone::regulationExemptionText() const
{
    return GeoZoneText::regulationExemption(_data);
}

QString GeoZone::verticalLimitsText() const
{
    return GeoZoneText::verticalLimits(_data);
}
