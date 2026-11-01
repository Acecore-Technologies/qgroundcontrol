/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneProviders.h"
#include "GeoZoneFormat.h"

#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

namespace {

/// DiPUL layers of aviation zones, small enough to download for the whole country
const QStringList kDipulAviationLayers = {
    QStringLiteral("kontrollzonen"), QStringLiteral("flughaefen"), QStringLiteral("flugplaetze"),
    QStringLiteral("flugbeschraenkungsgebiete"), QStringLiteral("temporaere_betriebseinschraenkungen"),
};

const QStringList kDipulAllLayers = {
    QStringLiteral("bahnanlagen"), QStringLiteral("behoerden"), QStringLiteral("binnenwasserstrassen"), QStringLiteral("bundesautobahnen"),
    QStringLiteral("bundesstrassen"), QStringLiteral("diplomatische_vertretungen"), QStringLiteral("flugbeschraenkungsgebiete"),
    QStringLiteral("flughaefen"), QStringLiteral("flugplaetze"), QStringLiteral("freibaeder"), QStringLiteral("industrieanlagen"),
    QStringLiteral("internationale_organisationen"), QStringLiteral("justizvollzugsanstalten"), QStringLiteral("kontrollzonen"),
    QStringLiteral("kraftwerke"), QStringLiteral("krankenhaeuser"), QStringLiteral("labore"), QStringLiteral("militaerische_anlagen"),
    QStringLiteral("nationalparks"), QStringLiteral("naturschutzgebiete"), QStringLiteral("polizei"), QStringLiteral("schifffahrtsanlagen"),
    QStringLiteral("seewasserstrassen"), QStringLiteral("sicherheitsbehoerden"), QStringLiteral("stromleitungen"),
    QStringLiteral("temporaere_betriebseinschraenkungen"), QStringLiteral("umspannwerke"), QStringLiteral("vogelschutzgebiete"),
    QStringLiteral("windkraftanlagen"), QStringLiteral("wohngrundstuecke"),
};

} // namespace

QList<GeoZoneProviders::Service> GeoZoneProviders::services()
{
    using Format = GeoZoneParser::Format;
    const auto feed = [](const QString &id, const QString &name, const QString &host, Format format, const QString &url) {
        Service service;
        service.id = id;
        service.name = name;
        service.host = host;
        service.format = format;
        service.feedUrl = url;
        return service;
    };

    Service dipulAviation = feed(QStringLiteral("dipul-aviation"), tr("Germany aviation zones (DiPUL)"), QStringLiteral("uas-betrieb.de"), Format::Dipul,
                                 _dipulUrl(kDipulAviationLayers, QGeoRectangle()));

    Service dipulAll = feed(QStringLiteral("dipul"), tr("Germany, all zones (DiPUL)"), QStringLiteral("uas-betrieb.de"), Format::Dipul, QString());
    dipulAll.coverage = QGeoRectangle(QGeoCoordinate(55.2, 5.8), QGeoCoordinate(47.2, 15.1));
    dipulAll.areaUrl = [](const QGeoRectangle &area) { return _dipulUrl(kDipulAllLayers, area); };

    Service france = feed(QStringLiteral("france"), tr("France, open category (IGN)"), QStringLiteral("data.geopf.fr"), Format::FranceUas, QString());
    france.coverage = QGeoRectangle(QGeoCoordinate(51.2, -5.3), QGeoCoordinate(41.3, 9.7));
    france.areaUrl = [](const QGeoRectangle &area) {
        return QStringLiteral("https://data.geopf.fr/wfs/ows?SERVICE=WFS&VERSION=2.0.0&REQUEST=GetFeature&TYPENAMES=TRANSPORTS.DRONES.RESTRICTIONS:carte_restriction_drones_lf"
                              "&OUTPUTFORMAT=application/json&BBOX=") + _bboxQuery(area, QStringLiteral(",EPSG:4326"));
    };

    Service nats = feed(QStringLiteral("nats"), tr("United Kingdom FRZ (NATS)"), QStringLiteral("ead-it.com"), Format::Aixm,
                        QStringLiteral("https://nats-uk.ead-it.com/cms-nats/opencms/en/Publications/digital-datasets/"));
    // NATS lists the full UAS flight restriction datasets of the current and next AIRAC cycles
    nats.datasetLinkPattern = QRegularExpression(QStringLiteral("href=\"([^\"]*UAS_AREA_1/[^\"]*_FULL_(\\d{8})_XML\\.zip)\""));
    nats.airac = true;

    return {
        feed(QStringLiteral("traficom"), tr("Finland (Traficom)"), QStringLiteral("traficom.fi"), Format::Unknown,
             QStringLiteral("https://eservices.traficom.fi/Ilmatilasovellus/api/uas-reservations/json?lang=en")),
        feed(QStringLiteral("lgs"), tr("Latvia (LGS)"), QStringLiteral("drz.lv"), Format::Unknown,
             QStringLiteral("https://drz.lv/api/v1/export-history/UASZoneVersion")),
        feed(QStringLiteral("geoportail-lu"), tr("Luxembourg (Geoportail)"), QStringLiteral("geoportail.lu"), Format::Unknown,
             QStringLiteral("https://drones.geoportail.lu/zones")),
        feed(QStringLiteral("lfv"), tr("Sweden (LFV)"), QStringLiteral("lfv.se"), Format::Unknown,
             QStringLiteral("https://dronechart.lfv.se/data/uas_zones_ED318.json")),
        feed(QStringLiteral("faa-tfr"), tr("United States TFRs (FAA)"), QStringLiteral("faa.gov"), Format::FaaTfr,
             QStringLiteral("https://tfr.faa.gov/geoserver/TFR/ows?service=WFS&version=1.1.0&request=GetFeature&typeName=TFR:V_TFR_LOC&outputFormat=application/json")),
        feed(QStringLiteral("foca"), tr("Switzerland (FOCA)"), QStringLiteral("admin.ch"), Format::Unknown,
             QStringLiteral("https://data.geo.admin.ch/ch.bazl.einschraenkungen-drohnen/einschraenkungen-drohnen/einschraenkungen-drohnen_4326.json")),
        dipulAviation,
        nats,
        dipulAll,
        france,
    };
}

QList<GeoZoneProviders::ServiceTemplate> GeoZoneProviders::serviceTemplates()
{
    QList<ServiceTemplate> templates;

    ServiceTemplate ed318;
    ed318.service.id = QStringLiteral("ed318");
    ed318.service.name = tr("ED-318 geozone service");
    ed318.service.method = GeoZoneService::Method::Ed318Post;
    ed318.service.areaOfInterest = true;
    ed318.note = tr("Enter the address of the UAS zones endpoint of a service following the ED-318 retrieval interface. "
                    "The zones around the mission and the vehicle are requested with their area and time.");
    templates.append(ed318);

    // The ED-318 retrieval interface of the DFS Geospatial Information Service, see its OpenAPI specification
    ServiceTemplate dfs;
    dfs.url = QStringLiteral("https://dipul-service.dfs.de/api/geofencing/v2/uas-zones");
    dfs.service.id = QStringLiteral("dfs-ed318");
    dfs.service.name = tr("Germany ED-318 service (DFS)");
    dfs.service.method = GeoZoneService::Method::Ed318Post;
    dfs.service.auth = GeoZoneService::Auth::Basic;
    dfs.service.areaOfInterest = true;
    dfs.service.maxAreaKm2 = 100;
    dfs.note = tr("Requires a dipul account, requested from DFS through the dipul contact form. The service answers areas up to 100 km².");
    templates.append(dfs);

    ServiceTemplate area;
    area.service.id = QStringLiteral("area");
    area.service.name = tr("Zones of the area (GET)");
    area.service.areaOfInterest = true;
    area.note = tr("Enter an address with {bbox} (west,south,east,north), {west}, {south}, {east}, {north}, {lat}, {lon}, {radiusKm}, "
                   "{start} or {end}, filled with the area around the mission and the vehicle.");
    templates.append(area);

    // The FAA NOTAM Management Service publishes NOTAMs as GeoJSON or AIXM 5.1 events to registered clients
    ServiceTemplate nms;
    nms.url = QStringLiteral("https://api-nms.aim.faa.gov/nmsapi/v1/notams?latitude={lat}&longitude={lon}&radius={radiusNm}&feature=AIRSPACE");
    nms.service.id = QStringLiteral("faa-nms");
    nms.service.name = tr("United States NOTAMs (FAA NMS)");
    nms.service.auth = GeoZoneService::Auth::OAuth2ClientCredentials;
    nms.service.tokenUrl = QStringLiteral("https://api-nms.aim.faa.gov/v1/auth/token");
    nms.service.headers = { { QStringLiteral("nmsResponseFormat"), QStringLiteral("GEOJSON") } };
    nms.service.responseMember = QStringLiteral("data.geojson");
    nms.service.areaOfInterest = true;
    nms.service.timeWindowHours = 24;
    nms.note = tr("Configuration only: QGroundControl does not read NOTAM GeoJSON yet. The FAA grants client credentials on request to NOTAMS@faa.gov.");
    nms.supported = false;
    templates.append(nms);

    return templates;
}

QVariantList GeoZoneProviders::serviceTemplateList()
{
    QVariantList list;
    for (const ServiceTemplate &serviceTemplate : serviceTemplates()) {
        list.append(QVariantMap{
            { QStringLiteral("id"), serviceTemplate.service.id },
            { QStringLiteral("name"), serviceTemplate.service.name },
            { QStringLiteral("url"), serviceTemplate.url },
            { QStringLiteral("note"), serviceTemplate.note },
            { QStringLiteral("supported"), serviceTemplate.supported },
            { QStringLiteral("service"), serviceTemplate.service.toVariantMap() },
        });
    }
    return list;
}

QVariantList GeoZoneProviders::feedPresets()
{
    QVariantList presets;
    for (const Service &service : services()) {
        if (!service.feedUrl.isEmpty()) {
            presets.append(QVariantMap{ { QStringLiteral("name"), service.name }, { QStringLiteral("url"), service.feedUrl } });
        }
    }
    return presets;
}

QVariantList GeoZoneProviders::areaProviders()
{
    QVariantList providers;
    for (const Service &service : services()) {
        if (service.areaUrl) {
            providers.append(QVariantMap{ { QStringLiteral("name"), service.name }, { QStringLiteral("id"), service.id },
                                          { QStringLiteral("coverage"), QVariant::fromValue(service.coverage) } });
        }
    }
    return providers;
}

QString GeoZoneProviders::areaFeedUrl(const QString &serviceId, const QGeoRectangle &area)
{
    if (!area.isValid() || (area.width() > maxAreaDegrees) || (area.height() > maxAreaDegrees)) {
        return QString();
    }
    for (const Service &service : services()) {
        if ((service.id == serviceId) && service.areaUrl) {
            return service.areaUrl(area);
        }
    }
    return QString();
}

GeoZoneParser::Format GeoZoneProviders::formatHint(const QString &url)
{
    const QString host = QUrl(url).host();
    for (const Service &service : services()) {
        if ((service.format != GeoZoneParser::Format::Unknown) &&
            ((host == service.host) || host.endsWith(QLatin1Char('.') + service.host))) {
            return service.format;
        }
    }
    return GeoZoneParser::Format::Unknown;
}

QString GeoZoneProviders::serviceId(const QString &url)
{
    const QList<Service> all = services();
    for (const Service &service : all) {
        if (!service.feedUrl.isEmpty() && (service.feedUrl == url)) {
            return service.id;
        }
    }
    // Services sharing a host are told apart by whether the URL is that of an area
    const QString host = QUrl(url).host();
    const bool area = url.contains(QStringLiteral("bbox="), Qt::CaseInsensitive);
    QString id;
    for (const Service &service : all) {
        if ((host == service.host) || host.endsWith(QLatin1Char('.') + service.host)) {
            if (id.isEmpty() || (area && service.areaUrl)) {
                id = service.id;
            }
        }
    }
    return id;
}

QString GeoZoneProviders::datasetLink(const QString &url, const QByteArray &data, const QDate &today)
{
    const QByteArray start = data.left(1024).trimmed().toLower();
    if (!start.startsWith("<!doctype html") && !start.startsWith("<html")) {
        return QString();
    }

    const QString page = QString::fromUtf8(data);
    for (const Service &service : services()) {
        if (!service.datasetLinkPattern.isValid() || service.datasetLinkPattern.pattern().isEmpty()) {
            continue;
        }
        QString link;
        QDate linkDate;
        QString earliestLink;
        QDate earliestDate;
        QRegularExpressionMatchIterator it = service.datasetLinkPattern.globalMatch(page);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            const QDate date = QDate::fromString(match.captured(2), QStringLiteral("yyyyMMdd"));
            if (!date.isValid()) {
                continue;
            }
            if ((date <= today) && (!linkDate.isValid() || (date > linkDate))) {
                link = match.captured(1);
                linkDate = date;
            }
            if (!earliestDate.isValid() || (date < earliestDate)) {
                earliestLink = match.captured(1);
                earliestDate = date;
            }
        }
        if (link.isEmpty()) {
            link = earliestLink;
        }
        if (!link.isEmpty()) {
            return QUrl(url).resolved(QUrl(link)).toString();
        }
    }
    return QString();
}

QDate GeoZoneProviders::airacDatasetDate(const QString &datasetUrl)
{
    if (datasetUrl.isEmpty()) {
        return QDate();
    }
    // The patterns match the links of the download pages
    const QString link = QStringLiteral("href=\"%1\"").arg(datasetUrl);
    for (const Service &service : services()) {
        if (!service.airac || !service.datasetLinkPattern.isValid() || service.datasetLinkPattern.pattern().isEmpty()) {
            continue;
        }
        const QRegularExpressionMatch match = service.datasetLinkPattern.match(link);
        if (match.hasMatch()) {
            return QDate::fromString(match.captured(2), QStringLiteral("yyyyMMdd"));
        }
    }
    return QDate();
}

bool GeoZoneProviders::isLive(GeoZoneParser::Format format)
{
    const GeoZoneFormat *const found = GeoZoneFormat::find(format);
    return found && found->isLive();
}

QString GeoZoneProviders::_bboxQuery(const QGeoRectangle &area, const QString &suffix)
{
    return QStringLiteral("%1,%2,%3,%4").arg(area.topLeft().longitude(), 0, 'f', 5).arg(area.bottomRight().latitude(), 0, 'f', 5)
                                         .arg(area.bottomRight().longitude(), 0, 'f', 5).arg(area.topLeft().latitude(), 0, 'f', 5) + suffix;
}

QString GeoZoneProviders::_dipulUrl(const QStringList &layers, const QGeoRectangle &area)
{
    QStringList typeNames;
    for (const QString &layer : layers) {
        typeNames.append(QStringLiteral("dipul:") + layer);
    }
    // WFS 1.0.0 is the version of the DiPUL server which filters several layers by a bounding box
    QString url = QStringLiteral("https://uas-betrieb.de/geoservices/dipul/ows?service=WFS&version=1.0.0&request=GetFeature&outputFormat=application/json&typeName=%1")
                      .arg(typeNames.join(QLatin1Char(',')));
    if (area.isValid()) {
        url += QStringLiteral("&BBOX=") + _bboxQuery(area);
    }
    return url;
}

GeoZoneProviders::WfsCounts GeoZoneProviders::wfsCounts(const QByteArray &head, const QByteArray &tail)
{
    const auto count = [&head, &tail](const char *name) -> qint64 {
        const QRegularExpression pattern(QStringLiteral("\"%1\"\\s*:\\s*(\\d+)").arg(QLatin1String(name)));
        for (const QByteArray *bytes : { &head, &tail }) {
            const QRegularExpressionMatch match = pattern.match(QString::fromLatin1(*bytes));
            if (match.hasMatch()) {
                return match.captured(1).toLongLong();
            }
        }
        return -1;
    };
    WfsCounts counts;
    counts.matched = count("numberMatched");
    if (counts.matched < 0) {
        counts.matched = count("totalFeatures");
    }
    counts.returned = count("numberReturned");
    return counts;
}

QString GeoZoneProviders::wfsPageUrl(const QString &url, qint64 startIndex)
{
    QUrl pageUrl(url);
    QUrlQuery query(pageUrl);
    bool getFeature = false;
    QList<std::pair<QString, QString>> items;
    for (const auto &[key, value] : query.queryItems(QUrl::FullyDecoded)) {
        if (key.compare(QStringLiteral("request"), Qt::CaseInsensitive) == 0) {
            getFeature = (value.compare(QStringLiteral("GetFeature"), Qt::CaseInsensitive) == 0);
        }
        if (key.compare(QStringLiteral("startIndex"), Qt::CaseInsensitive) != 0) {
            items.append({ key, value });
        }
    }
    if (!getFeature) {
        return QString();
    }
    items.append({ QStringLiteral("STARTINDEX"), QString::number(startIndex) });
    query.setQueryItems(items);
    pageUrl.setQuery(query);
    return pageUrl.toString();
}
