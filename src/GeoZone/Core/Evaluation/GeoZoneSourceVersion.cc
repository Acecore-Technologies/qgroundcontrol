/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneSourceVersion.h"
#include "GeoZoneAirac.h"
#include "GeoZoneBox.h"
#include "GeoZoneSource.h"

#include <cmath>

namespace {

QJsonValue dateText(const QDateTime &dateTime)
{
    return dateTime.isValid() ? QJsonValue(dateTime.toUTC().toString(Qt::ISODate)) : QJsonValue(QJsonValue::Null);
}

QDateTime dateFromText(const QJsonValue &value)
{
    QDateTime dateTime = QDateTime::fromString(value.toString(), Qt::ISODate);
    return dateTime.isValid() ? dateTime.toUTC() : QDateTime();
}

} // namespace

GeoZoneSourceVersion GeoZoneSourceVersion::fromSource(const GeoZoneSource *source)
{
    GeoZoneSourceVersion version;
    if (!source) {
        return version;
    }
    version.key = source->key();
    version.name = source->name();
    version.format = source->formatName();
    version.provider = source->provider();
    version.dataDate = source->dataDate().isValid() ? source->dataDate().toUTC() : QDateTime();
    version.loadedAt = source->loadedAt().isValid() ? source->loadedAt().toUTC() : QDateTime();
    version.checkedAt = source->checkedAt().isValid() ? source->checkedAt().toUTC() : QDateTime();
    version.validFrom = source->validFrom().isValid() ? source->validFrom().toUTC() : QDateTime();
    version.validTo = source->validTo().isValid() ? source->validTo().toUTC() : QDateTime();
    // Live restrictions change between the cycles
    if (!source->isLive()) {
        version.airacCycle = airacCycleOf(version.dataDate.date());
    }
    version.contentHash = source->contentHash();
    return version;
}

QList<GeoZoneSourceVersion> GeoZoneSourceVersion::covering(const QList<const GeoZoneSource*> &sources, const GeoZoneBox &area)
{
    QList<GeoZoneSourceVersion> versions;
    for (const GeoZoneSource *source : sources) {
        if (source && source->enabled() && source->loaded() && (!area.isValid() || source->box().intersects(area))) {
            versions.append(fromSource(source));
        }
    }
    return versions;
}

QString GeoZoneSourceVersion::airacCycleOf(const QDate &date)
{
    return GeoZoneAirac::cycleName(date);
}

bool GeoZoneSourceVersion::sameData(const GeoZoneSourceVersion &other) const
{
    if (!contentHash.isEmpty() && !other.contentHash.isEmpty()) {
        return contentHash == other.contentHash;
    }
    return dataDate == other.dataDate;
}

QJsonObject GeoZoneSourceVersion::toJson() const
{
    QJsonObject json;
    json.insert(QStringLiteral("key"), key);
    json.insert(QStringLiteral("name"), name);
    json.insert(QStringLiteral("format"), format);
    json.insert(QStringLiteral("provider"), provider);
    json.insert(QStringLiteral("dataDate"), dateText(dataDate));
    json.insert(QStringLiteral("loaded"), dateText(loadedAt));
    json.insert(QStringLiteral("checked"), dateText(checkedAt));
    json.insert(QStringLiteral("validFrom"), dateText(validFrom));
    json.insert(QStringLiteral("validTo"), dateText(validTo));
    json.insert(QStringLiteral("airacCycle"), airacCycle);
    json.insert(QStringLiteral("contentHash"), QString::fromLatin1(contentHash.toHex()));
    return json;
}

GeoZoneSourceVersion GeoZoneSourceVersion::fromJson(const QJsonObject &json)
{
    GeoZoneSourceVersion version;
    version.key = json.value(QStringLiteral("key")).toString();
    version.name = json.value(QStringLiteral("name")).toString();
    version.format = json.value(QStringLiteral("format")).toString();
    version.provider = json.value(QStringLiteral("provider")).toString();
    version.dataDate = dateFromText(json.value(QStringLiteral("dataDate")));
    version.loadedAt = dateFromText(json.value(QStringLiteral("loaded")));
    version.checkedAt = dateFromText(json.value(QStringLiteral("checked")));
    version.validFrom = dateFromText(json.value(QStringLiteral("validFrom")));
    version.validTo = dateFromText(json.value(QStringLiteral("validTo")));
    version.airacCycle = json.value(QStringLiteral("airacCycle")).toString();
    version.contentHash = QByteArray::fromHex(json.value(QStringLiteral("contentHash")).toString().toLatin1());
    return version;
}
