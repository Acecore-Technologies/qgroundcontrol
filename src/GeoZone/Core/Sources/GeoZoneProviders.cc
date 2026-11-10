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

#include <algorithm>

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

/// The FAA's public ArcGIS feature services, see https://udds-faa.opendata.arcgis.com
const QString kFaaHost = QStringLiteral("services6.arcgis.com");
const QString kFaaPath = QStringLiteral("/ssFJjBXIUyZDrSYZ/arcgis/rest/services/");

struct FaaLayer {
    const char *id;
    const char *service;
    const char *where;
    const char *fields;
};

/// The layers of the FAA airspace providers: UAS facility maps, national security UAS flight restrictions, the
/// controlled airspace classes reaching the surface and special use airspace
const FaaLayer kFaaLayers[] = {
    { "faa-uasfm", "FAA_UAS_FacilityMap_Data", "1=1",
      "OBJECTID,GLOBALID,CEILING,UNIT,MAP_EFF,ARPT_COUNT,APT1_FAAID,APT1_ICAO,APT1_NAME,APT1_LAANC,APT2_ICAO,APT2_NAME,APT2_LAANC,AIRSPACE_1,AIRSPACE_2" },
    { "faa-nsufr", "DoD_Mar_13", "1=1", "*" },
    { "faa-class", "Class_Airspace", "TYPE_CODE='CLASS' AND LOCAL_TYPE IN ('CLASS_B','CLASS_C','CLASS_D','CLASS_E2','CLASS_E3','CLASS_E4')", "*" },
    { "faa-sua", "Special_Use_Airspace", "1=1", "*" },
};

/// The 50 states and the territories in the Caribbean
const QGeoRectangle kUnitedStates(QGeoCoordinate(72.0, -180.0), QGeoCoordinate(17.5, -64.5));

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

    // ENAIRE publishes the Spanish zones as ED-318 downloads, updated on AIRAC dates, and its many infrastructure
    // zones (mostly railways) by area through the feature service of its AIS
    Service enaireAero = feed(QStringLiteral("enaire-aero"), tr("Spain aeronautical zones (ENAIRE)"), QStringLiteral("enaire.es"), Format::ED318,
                              QStringLiteral("https://aip.enaire.es/recursos/descargas/ZGUAS/ZGUAS_Aero.zip"));
    enaireAero.path = QStringLiteral("/recursos/descargas/ZGUAS/");
    Service enaireUrban = feed(QStringLiteral("enaire-urban"), tr("Spain urban zones (ENAIRE)"), QStringLiteral("enaire.es"), Format::ED318,
                               QStringLiteral("https://aip.enaire.es/recursos/descargas/ZGUAS/ZGUAS_Urbano.zip"));
    enaireUrban.path = enaireAero.path;
    Service enaireInfra = feed(QStringLiteral("enaire-infra"), tr("Spain infrastructure zones (ENAIRE)"), QStringLiteral("enaire.es"), Format::ED318, QString());
    enaireInfra.path = QStringLiteral("/insigniads/rest/services/NSF_SRV/SRV_UAS_ZG_data_V2/");
    enaireInfra.coverage = QGeoRectangle(QGeoCoordinate(44.0, -18.5), QGeoCoordinate(27.5, 4.5));
    enaireInfra.areaUrl = [](const QGeoRectangle &area) {
        return QStringLiteral("https://servais.enaire.es/insigniads/rest/services/NSF_SRV/SRV_UAS_ZG_data_V2/FeatureServer/0/query?where=1%3D1&geometry=%1"
                              "&geometryType=esriGeometryEnvelope&inSR=4326&spatialRel=esriSpatialRelIntersects&outFields=*&outSR=4326&geometryPrecision=6"
                              "&f=geojson&resultRecordCount=2000&resultOffset=0").arg(_bboxQuery(area));
    };

    // The ministry IenW links the current ED-269 file of the Dutch zones from its page on drones
    Service netherlands = feed(QStringLiteral("ienw"), tr("Netherlands (IenW)"), QStringLiteral("luchtvaartindetoekomst.nl"), Format::ED269,
                               QStringLiteral("https://www.luchtvaartindetoekomst.nl/onderwerpen/o/onbemande-luchtvaart"));
    netherlands.datasetLinkPattern = QRegularExpression(QStringLiteral("<a\\b(?=[^>]*JSON)[^>]*href=\"(https://www\\.nieuwsienw\\.nl/api/documents/downloadfile\\?[^\"]+)\""));

    // Austro Control lists its ED-269 datasets with the date each takes effect, the test datasets aside
    Service austria = feed(QStringLiteral("austro-control"), tr("Austria (Austro Control)"), QStringLiteral("austrocontrol.at"), Format::ED269,
                           QStringLiteral("https://www.austrocontrol.at/luftfahrtbehoerde/lizenzen__bewilligungen/drohnen/geografische_zonen"));
    austria.datasetLinkPattern = QRegularExpression(QStringLiteral("href=\"([^\"]*/UASZones-ACG-\\d{8}-\\d{6}-full(?![^\"]*test)[^\"]*\\.(?:zip|json))\""
                                                                   "(?:(?!</tr>)[\\s\\S])*?<td[^>]*>\\s*(?:<span[^>]*>)?\\s*(\\d{1,2}\\.\\d{1,2}\\.\\d{4})"));
    austria.datasetDateFormat = QStringLiteral("d.M.yyyy");

    const QStringList faaNames = {
        tr("United States UAS facility maps (FAA)"), tr("United States national security UAS restrictions (FAA)"),
        tr("United States controlled airspace (FAA)"), tr("United States special use airspace (FAA)"),
    };
    QList<Service> faa;
    qsizetype nameIndex = 0;
    for (const FaaLayer &layer : kFaaLayers) {
        Service service = feed(QString::fromLatin1(layer.id), faaNames.value(nameIndex++), kFaaHost, Format::FaaAirspace, QString());
        service.path = kFaaPath + QString::fromLatin1(layer.service) + QLatin1Char('/');
        service.coverage = kUnitedStates;
        service.areaUrl = [layer](const QGeoRectangle &area) {
            return _faaUrl(QString::fromLatin1(layer.service), QString::fromLatin1(layer.where), QString::fromLatin1(layer.fields), _bboxQuery(area));
        };
        faa.append(service);
    }

    return QList<Service>{
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
        enaireAero,
        enaireUrban,
        netherlands,
        austria,
        dipulAll,
        france,
        enaireInfra,
    } + faa;
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
                    "The zones around the mission, the vehicle and the map view are requested with their area and time.");
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
                   "{start} or {end}, filled with the area around the mission, the vehicle and the map view.");
    templates.append(area);

    // The FAA NOTAM Management Service isn't offered until its NOTAM GeoJSON can be read

    // OpenAIP airspaces, unofficial community data, by area with an API key or by country without
    ServiceTemplate openAip;
    openAip.url = QStringLiteral("https://api.core.openaip.net/api/airspaces?bbox={bbox}&limit=1000&page=1");
    openAip.service.id = QStringLiteral("openaip");
    openAip.service.name = tr("Airspaces (OpenAIP, unofficial)");
    openAip.service.auth = GeoZoneService::Auth::ApiKeyHeader;
    openAip.service.authParameter = QStringLiteral("x-openaip-api-key");
    openAip.service.responseMember = QStringLiteral("items");
    openAip.service.areaOfInterest = true;
    // The API answers areas up to 5 degrees wide and high
    openAip.service.maxAreaKm2 = 40000;
    openAip.format = GeoZoneParser::Format::OpenAip;
    openAip.note = tr("Unofficial community data under CC BY-NC 4.0 (non-commercial use, attribution to www.openaip.net), not for navigation: "
                      "check the official drone zones. Enter the API key of an API client created in your openaip.net profile.");
    templates.append(openAip);

    ServiceTemplate openAipExport;
    openAipExport.url = QStringLiteral("https://storage.openaip.net/openaip-system-exports/ch_asp.geojson");
    openAipExport.service.id = QStringLiteral("openaip-export");
    // Its sources are named after the country of their data, see GeoZoneOpenAipFormat
    openAipExport.name = tr("Airspaces of a country (OpenAIP, unofficial)");
    openAipExport.format = GeoZoneParser::Format::OpenAip;
    openAipExport.note = tr("The daily export of a country's airspaces, without an account: replace ch with the two-letter code of the country. "
                            "Unofficial community data under CC BY-NC 4.0 (non-commercial use, attribution to www.openaip.net), not for navigation.");
    templates.append(openAipExport);

    // The Airspace Link AirHub API briefs an area posted as GeoJSON with OAuth2 client credentials and an API key header
    ServiceTemplate airspaceLink;
    airspaceLink.url = QStringLiteral("https://airhub-api.airspacelink.com/v2/briefing/b4ufly/area");
    airspaceLink.service.id = QStringLiteral("airspacelink");
    airspaceLink.service.name = tr("United States advisories (Airspace Link)");
    airspaceLink.service.auth = GeoZoneService::Auth::OAuth2ClientCredentials;
    airspaceLink.service.tokenUrl = QStringLiteral("https://airhub-api.airspacelink.com/v1/oauth/token");
    airspaceLink.service.scope = QStringLiteral("briefing:area:read");
    airspaceLink.service.areaOfInterest = true;
    airspaceLink.note = tr("Configuration only: QGroundControl does not post GeoJSON areas or read Airspace Link notices yet, and the API also takes "
                           "an API key header (x-api-key). Airspace Link grants the key and client credentials on request (developers.airspacelink.com).");
    airspaceLink.supported = false;
    templates.append(airspaceLink);

    // The FAA's public airspace layers, also offered for the map area, following the area of interest
    const QList<Service> all = services();
    for (const FaaLayer &layer : kFaaLayers) {
        const auto service = std::find_if(all.cbegin(), all.cend(), [&layer](const Service &candidate) { return candidate.id == QLatin1String(layer.id); });
        ServiceTemplate faa;
        faa.url = _faaUrl(QString::fromLatin1(layer.service), QString::fromLatin1(layer.where), QString::fromLatin1(layer.fields), QStringLiteral("{bbox}"));
        faa.service.id = service->id;
        faa.service.name = service->name;
        faa.service.areaOfInterest = true;
        // Facility maps are grids of cells of about 900 m, many around large airports
        faa.service.maxAreaKm2 = (faa.service.id == QStringLiteral("faa-uasfm")) ? 10000 : 0;
        faa.note = tr("Public FAA data, no account needed. Informational: check B4UFLY or a LAANC provider before flying.");
        templates.append(faa);
    }

    return templates;
}

QVariantList GeoZoneProviders::serviceTemplateList()
{
    ServiceTemplate other;
    other.service.name = tr("Other service");
    other.note = tr("Enter the address of the service and how it is accessed.");
    QList<ServiceTemplate> templates = serviceTemplates();
    templates.append(other);
    QVariantList list;
    for (const ServiceTemplate &serviceTemplate : std::as_const(templates)) {
        list.append(QVariantMap{
            { QStringLiteral("name"), serviceTemplate.service.name.isEmpty() ? serviceTemplate.name : serviceTemplate.service.name },
            { QStringLiteral("url"), serviceTemplate.url },
            { QStringLiteral("note"), serviceTemplate.note },
            { QStringLiteral("supported"), serviceTemplate.supported },
            { QStringLiteral("form"), serviceTemplate.service.toForm() },
        });
    }
    return list;
}

GeoZoneService GeoZoneProviders::serviceFromForm(int templateIndex, const QVariantMap &form)
{
    const QList<ServiceTemplate> templates = serviceTemplates();
    return (((templateIndex >= 0) && (templateIndex < templates.count())) ? templates[templateIndex].service : GeoZoneService()).withForm(form);
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

QGeoRectangle GeoZoneProviders::feedArea(const QString &url)
{
    const QUrlQuery query{QUrl(url)};
    for (const QPair<QString, QString> &item : query.queryItems(QUrl::FullyDecoded)) {
        // ArcGIS feature services take the area as an envelope geometry
        if ((item.first.compare(QStringLiteral("bbox"), Qt::CaseInsensitive) != 0) &&
            ((item.first != QStringLiteral("geometry")) || (query.queryItemValue(QStringLiteral("geometryType")) != QStringLiteral("esriGeometryEnvelope")))) {
            continue;
        }
        // West, south, east and north, see _bboxQuery
        const QStringList values = item.second.split(QLatin1Char(','));
        if (values.count() < 4) {
            return QGeoRectangle();
        }
        double bounds[4];
        for (int i = 0; i < 4; i++) {
            bool ok = false;
            bounds[i] = values[i].toDouble(&ok);
            if (!ok) {
                return QGeoRectangle();
            }
        }
        const QGeoRectangle area(QGeoCoordinate(bounds[3], bounds[0]), QGeoCoordinate(bounds[1], bounds[2]));
        return area.isValid() ? area : QGeoRectangle();
    }
    return QGeoRectangle();
}

bool GeoZoneProviders::areaDownloaded(const QString &serviceId, const QStringList &feedUrls, const QGeoRectangle &area)
{
    if (!area.isValid()) {
        return false;
    }
    // The URLs round the bounds to 5 decimals
    constexpr double tolerance = 1e-5;
    for (const QString &url : feedUrls) {
        if (GeoZoneProviders::serviceId(url) != serviceId) {
            continue;
        }
        const QGeoRectangle downloaded = feedArea(url);
        if (downloaded.isValid() &&
            (downloaded.topLeft().latitude() + tolerance >= area.topLeft().latitude()) &&
            (downloaded.bottomRight().latitude() - tolerance <= area.bottomRight().latitude()) &&
            (downloaded.topLeft().longitude() - tolerance <= area.topLeft().longitude()) &&
            (downloaded.bottomRight().longitude() + tolerance >= area.bottomRight().longitude())) {
            return true;
        }
    }
    return false;
}

bool GeoZoneProviders::_matches(const Service &service, const QUrl &url)
{
    const QString host = url.host();
    return ((host == service.host) || host.endsWith(QLatin1Char('.') + service.host)) &&
           (service.path.isEmpty() || url.path().startsWith(service.path, Qt::CaseInsensitive));
}

GeoZoneParser::Format GeoZoneProviders::formatHint(const QString &url)
{
    const QUrl parsedUrl(url);
    for (const Service &service : services()) {
        if ((service.format != GeoZoneParser::Format::Unknown) && _matches(service, parsedUrl)) {
            return service.format;
        }
    }
    for (const ServiceTemplate &serviceTemplate : serviceTemplates()) {
        if ((serviceTemplate.format != GeoZoneParser::Format::Unknown) && (QUrl(serviceTemplate.url).host() == parsedUrl.host())) {
            return serviceTemplate.format;
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
    // Services sharing a host are told apart by their path, or by whether the URL is that of an area
    const QUrl parsedUrl(url);
    const bool area = url.contains(QStringLiteral("bbox="), Qt::CaseInsensitive) || url.contains(QStringLiteral("geometry="));
    QString id;
    for (const Service &service : all) {
        if (_matches(service, parsedUrl)) {
            if (id.isEmpty() || (area && service.areaUrl)) {
                id = service.id;
            }
        }
    }
    return id;
}

QString GeoZoneProviders::serviceName(const QString &url)
{
    // Other URLs of a service's server needn't be its data
    const QString id = feedArea(url).isValid() ? serviceId(url) : QString();
    for (const Service &service : services()) {
        if ((!service.feedUrl.isEmpty() && (service.feedUrl == url)) || (!id.isEmpty() && (service.id == id) && service.areaUrl)) {
            return service.name;
        }
    }
    return QString();
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
        // Links in HTML attributes escape their ampersands
        const auto resolved = [&url](const QString &link) {
            return QUrl(url).resolved(QUrl(QString(link).replace(QStringLiteral("&amp;"), QStringLiteral("&")))).toString();
        };
        if (service.datasetLinkPattern.captureCount() < 2) {
            const QRegularExpressionMatch match = service.datasetLinkPattern.match(page);
            if (match.hasMatch() && !match.captured(1).isEmpty()) {
                return resolved(match.captured(1));
            }
            continue;
        }
        QString link;
        QDate linkDate;
        QString earliestLink;
        QDate earliestDate;
        QRegularExpressionMatchIterator it = service.datasetLinkPattern.globalMatch(page);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            const QDate date = QDate::fromString(match.captured(2), service.datasetDateFormat);
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
            return resolved(link);
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
            return QDate::fromString(match.captured(2), service.datasetDateFormat);
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

QString GeoZoneProviders::_faaUrl(const QString &service, const QString &where, const QString &fields, const QString &bbox)
{
    // Pages of 1000 features, the most each of the services answers, see paging()
    return QStringLiteral("https://") + kFaaHost + kFaaPath + service +
           QStringLiteral("/FeatureServer/0/query?where=%1&geometry=%2&geometryType=esriGeometryEnvelope&inSR=4326&spatialRel=esriSpatialRelIntersects"
                          "&outFields=%3&outSR=4326&geometryPrecision=6&f=geojson&resultRecordCount=1000&resultOffset=0")
               .arg(QString::fromLatin1(QUrl::toPercentEncoding(where)), bbox, fields);
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

qint64 GeoZoneProviders::_jsonCount(const QByteArray &head, const QByteArray &tail, const char *name)
{
    const QRegularExpression pattern(QStringLiteral("\"%1\"\\s*:\\s*(\\d+)").arg(QLatin1String(name)));
    for (const QByteArray *bytes : { &head, &tail }) {
        const QRegularExpressionMatch match = pattern.match(QString::fromLatin1(*bytes));
        if (match.hasMatch()) {
            return match.captured(1).toLongLong();
        }
    }
    return -1;
}

GeoZoneProviders::WfsCounts GeoZoneProviders::wfsCounts(const QByteArray &head, const QByteArray &tail)
{
    WfsCounts counts;
    counts.matched = _jsonCount(head, tail, "numberMatched");
    if (counts.matched < 0) {
        counts.matched = _jsonCount(head, tail, "totalFeatures");
    }
    counts.returned = _jsonCount(head, tail, "numberReturned");
    return counts;
}

bool GeoZoneProviders::arcGisExceeded(const QByteArray &head, const QByteArray &tail)
{
    static const QRegularExpression pattern(QStringLiteral("\"exceededTransferLimit\"\\s*:\\s*true"));
    return pattern.match(QString::fromLatin1(tail)).hasMatch() || pattern.match(QString::fromLatin1(head)).hasMatch();
}

GeoZoneProviders::Paging GeoZoneProviders::paging(const QString &url, const QByteArray &head, const QByteArray &tail)
{
    Paging paging;
    const WfsCounts counts = wfsCounts(head, tail);
    if ((counts.returned > 0) && (counts.matched > counts.returned) && !wfsPageUrl(url, counts.returned).isEmpty()) {
        paging.scheme = Paging::Scheme::WfsStartIndex;
        paging.pageSize = counts.returned;
        paging.pageCount = static_cast<int>((std::min)((counts.matched - 1) / counts.returned, qint64{ maxWfsPages }));
        return paging;
    }

    const QUrlQuery query{QUrl(url)};
    const auto queryValue = [&query](const QString &name) {
        for (const auto &[key, value] : query.queryItems(QUrl::FullyDecoded)) {
            if (key.compare(name, Qt::CaseInsensitive) == 0) {
                return value;
            }
        }
        return QString();
    };
    // ArcGIS doesn't count the features, the page size is the one requested
    if (arcGisExceeded(head, tail)) {
        const qint64 pageSize = queryValue(QStringLiteral("resultRecordCount")).toLongLong();
        if (pageSize > 0) {
            paging.scheme = Paging::Scheme::ArcGisOffset;
            paging.pageSize = pageSize;
            paging.pageCount = maxWfsPages;
        }
        return paging;
    }

    const qint64 totalPages = _jsonCount(head, tail, "totalPages");
    const qint64 page = (std::max)(qint64{ 1 }, _jsonCount(head, tail, "page"));
    if (totalPages > page) {
        paging.scheme = Paging::Scheme::PageNumber;
        paging.pageCount = static_cast<int>((std::min)(totalPages - page, qint64{ maxWfsPages }));
    }
    return paging;
}

QString GeoZoneProviders::pageUrl(const QString &url, const Paging &paging, int page)
{
    const QUrlQuery query{QUrl(url)};
    switch (paging.scheme) {
    case Paging::Scheme::WfsStartIndex:
        return wfsPageUrl(url, paging.pageSize * page);
    case Paging::Scheme::ArcGisOffset: {
        const qint64 offset = query.queryItemValue(QStringLiteral("resultOffset")).toLongLong();
        return _withQueryItem(url, QStringLiteral("resultOffset"), QString::number(offset + (paging.pageSize * page)));
    }
    case Paging::Scheme::PageNumber: {
        const qint64 first = (std::max)(qint64{ 1 }, query.queryItemValue(QStringLiteral("page")).toLongLong());
        return _withQueryItem(url, QStringLiteral("page"), QString::number(first + page));
    }
    case Paging::Scheme::None:
        break;
    }
    return QString();
}

QString GeoZoneProviders::_withQueryItem(const QString &url, const QString &name, const QString &value)
{
    QUrl pageUrl(url);
    QUrlQuery query(pageUrl);
    QList<std::pair<QString, QString>> items;
    for (const auto &[key, itemValue] : query.queryItems(QUrl::FullyDecoded)) {
        if (key.compare(name, Qt::CaseInsensitive) != 0) {
            items.append({ key, itemValue });
        }
    }
    items.append({ name, value });
    query.setQueryItems(items);
    pageUrl.setQuery(query);
    return pageUrl.toString();
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
