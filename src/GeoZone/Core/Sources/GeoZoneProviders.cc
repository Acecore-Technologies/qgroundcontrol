/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneProviders.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneFeedCatalogue.h"
#include "GeoZoneFormat.h"
#include "GeoZoneSource.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QReadWriteLock>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

#include <algorithm>

namespace {

/// The catalogue in use, the built-in one until the repository applies its overlays
struct ActiveCatalogue {
    QReadWriteLock lock;
    bool initialized = false;
    QList<GeoZoneProviders::Service> services;
    QList<GeoZoneProviders::ServiceTemplate> templates;
};

ActiveCatalogue &activeCatalogue()
{
    static ActiveCatalogue catalogue;
    {
        const QReadLocker locker(&catalogue.lock);
        if (catalogue.initialized) {
            return catalogue;
        }
    }
    const GeoZoneFeedCatalogue::Catalogue builtIn = GeoZoneFeedCatalogue::builtIn();
    const QWriteLocker locker(&catalogue.lock);
    if (!catalogue.initialized) {
        catalogue.services = builtIn.feeds;
        catalogue.templates = builtIn.templates;
        catalogue.initialized = true;
    }
    return catalogue;
}

/// Names and notes of the built-in catalogue are translated when read, so that they follow the language
QString translated(const QString &text, bool translatable)
{
    return (translatable && !text.isEmpty()) ? QCoreApplication::translate(GeoZoneFeedCatalogue::translationContext, text.toUtf8().constData()) : text;
}

} // namespace

QString GeoZoneProviders::Service::areaUrl(const QGeoRectangle &area) const
{
    return areaUrlTemplate.isEmpty() ? QString() : GeoZoneService().requestUrl(areaUrlTemplate, area, GeoZoneEnvironment::currentDateTimeUtc());
}

QList<GeoZoneProviders::Service> GeoZoneProviders::_services()
{
    ActiveCatalogue &catalogue = activeCatalogue();
    const QReadLocker locker(&catalogue.lock);
    return catalogue.services;
}

QList<GeoZoneProviders::Service> GeoZoneProviders::services()
{
    QList<Service> services = _services();
    for (Service &service : services) {
        if (service.translatable) {
            service.name = translated(service.name, true);
            service.note = translated(service.note, true);
            service.templateNote = translated(service.templateNote, true);
        }
    }
    return services;
}

QList<GeoZoneProviders::ServiceTemplate> GeoZoneProviders::serviceTemplates()
{
    ActiveCatalogue &catalogue = activeCatalogue();
    QList<ServiceTemplate> templates;
    {
        const QReadLocker locker(&catalogue.lock);
        templates = catalogue.templates;
    }
    for (ServiceTemplate &serviceTemplate : templates) {
        if (serviceTemplate.translatable) {
            serviceTemplate.service.name = translated(serviceTemplate.service.name, true);
            serviceTemplate.name = translated(serviceTemplate.name, true);
            serviceTemplate.note = translated(serviceTemplate.note, true);
        }
    }

    // Area services also follow the area of interest, like the FAA's public layers
    for (const Service &service : services()) {
        if (!service.serviceTemplate || !service.servesAreas()) {
            continue;
        }
        ServiceTemplate serviceTemplate;
        serviceTemplate.url = service.areaUrlTemplate;
        serviceTemplate.service.id = service.id;
        serviceTemplate.service.name = service.name;
        serviceTemplate.service.areaOfInterest = true;
        serviceTemplate.service.maxAreaKm2 = service.templateMaxAreaKm2;
        serviceTemplate.note = service.templateNote;
        // The format comes from the service, whose path tells it apart from others on the same server
        serviceTemplate.origin = service.origin;
        templates.append(serviceTemplate);
    }
    return templates;
}

void GeoZoneProviders::setCatalogue(const QList<Service> &services, const QList<ServiceTemplate> &templates)
{
    ActiveCatalogue &catalogue = activeCatalogue();
    const QWriteLocker locker(&catalogue.lock);
    catalogue.services = services;
    catalogue.templates = templates;
}

void GeoZoneProviders::resetCatalogue()
{
    const GeoZoneFeedCatalogue::Catalogue builtIn = GeoZoneFeedCatalogue::builtIn();
    setCatalogue(builtIn.feeds, builtIn.templates);
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
    QList<Service> feeds;
    for (const Service &service : services()) {
        if (!service.feedUrl.isEmpty()) {
            feeds.append(service);
        }
    }
    std::stable_sort(feeds.begin(), feeds.end(), [](const Service &a, const Service &b) { return a.name.localeAwareCompare(b.name) < 0; });
    QVariantList presets;
    for (const Service &service : std::as_const(feeds)) {
        presets.append(QVariantMap{
            { QStringLiteral("id"), service.id }, { QStringLiteral("name"), service.name }, { QStringLiteral("url"), service.feedUrl },
            { QStringLiteral("country"), service.country }, { QStringLiteral("note"), service.note }, { QStringLiteral("licence"), service.licence },
            { QStringLiteral("attribution"), service.attribution }, { QStringLiteral("info"), service.info }, { QStringLiteral("origin"), service.origin },
        });
    }
    return presets;
}

QVariantList GeoZoneProviders::areaProviders()
{
    QVariantList providers;
    for (const Service &service : services()) {
        if (service.servesAreas()) {
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
    for (const Service &service : _services()) {
        if ((service.id == serviceId) && service.servesAreas()) {
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
        // West, south, east and north, see GeoZoneService::resolve
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
    // An entry without host, like the feed of a file, has no server whose addresses are its own: it would claim every
    // address without host
    if (service.host.isEmpty()) {
        return false;
    }
    const QString host = url.host();
    return ((host == service.host) || host.endsWith(QLatin1Char('.') + service.host)) &&
           (service.path.isEmpty() || url.path().startsWith(service.path, Qt::CaseInsensitive));
}

GeoZoneParser::Format GeoZoneProviders::formatHint(const QString &url)
{
    const QUrl parsedUrl(url);
    for (const Service &service : _services()) {
        if ((service.format != GeoZoneParser::Format::Unknown) && ((!service.feedUrl.isEmpty() && (service.feedUrl == url)) || _matches(service, parsedUrl))) {
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
    const QList<Service> all = _services();
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
            if (id.isEmpty() || (area && service.servesAreas())) {
                id = service.id;
            }
        }
    }
    return id;
}

QString GeoZoneProviders::serviceName(const QString &url)
{
    const QList<Service> all = services();
    const Service *const service = _serviceOf(all, url);
    return service ? service->name : QString();
}

const GeoZoneProviders::Service *GeoZoneProviders::_serviceOf(const QList<Service> &services, const QString &url)
{
    // Other URLs of a service's server needn't be its data
    const QString id = feedArea(url).isValid() ? serviceId(url) : QString();
    for (const Service &service : services) {
        if ((!service.feedUrl.isEmpty() && (service.feedUrl == url)) || (!id.isEmpty() && (service.id == id) && service.servesAreas())) {
            return &service;
        }
    }
    return nullptr;
}

QRegularExpression GeoZoneProviders::linkPattern(const QString &pattern)
{
    return QRegularExpression(QStringLiteral("(*LIMIT_MATCH=%1)").arg(linkMatchLimit) + pattern);
}

namespace {

/// @return The service whose download page the URL is, nullptr for other URLs
const GeoZoneProviders::Service *linkingService(const QList<GeoZoneProviders::Service> &services, const QString &url)
{
    for (const GeoZoneProviders::Service &service : services) {
        if (!service.feedUrl.isEmpty() && (service.feedUrl == url) && service.datasetLinkPattern.isValid() && !service.datasetLinkPattern.pattern().isEmpty()) {
            return &service;
        }
    }
    return nullptr;
}

} // namespace

bool GeoZoneProviders::hasDatasetLink(const QString &url)
{
    const QList<Service> services = _services();
    return linkingService(services, url) != nullptr;
}

QString GeoZoneProviders::datasetLink(const QString &url, const QByteArray &data, const QDate &today, QString *errorString)
{
    if (errorString) {
        errorString->clear();
    }
    // The pattern of a service is for its own page: those of a catalogue never run on the pages of other feeds
    const QList<Service> services = _services();
    const Service *const service = linkingService(services, url);
    if (!service || (data.size() > maxLinkPageBytes)) {
        return QString();
    }
    const QByteArray start = data.left(1024).trimmed().toLower();
    if (!start.startsWith("<!doctype html") && !start.startsWith("<html")) {
        return QString();
    }

    const QString page = QString::fromUtf8(data);
    QString link;
    if (service->datasetLinkPattern.captureCount() < 2) {
        const QRegularExpressionMatch match = service->datasetLinkPattern.match(page);
        if (match.hasMatch()) {
            link = match.captured(1);
        }
    } else {
        QDate linkDate;
        QString earliestLink;
        QDate earliestDate;
        QElapsedTimer timer;
        timer.start();
        QRegularExpressionMatchIterator it = service->datasetLinkPattern.globalMatch(page);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            const QDate date = QDate::fromString(match.captured(2), service->datasetDateFormat);
            if (date.isValid() && (date <= today) && (!linkDate.isValid() || (date > linkDate))) {
                link = match.captured(1);
                linkDate = date;
            }
            if (date.isValid() && (!earliestDate.isValid() || (date < earliestDate))) {
                earliestLink = match.captured(1);
                earliestDate = date;
            }
            if (timer.hasExpired(linkMatchBudgetMSecs)) {
                qCWarning(GeoZoneSourceLog) << "The search of the download page of" << service->id << "for its datasets took too long and was cut short";
                break;
            }
        }
        if (link.isEmpty()) {
            link = earliestLink;
        }
    }
    if (link.isEmpty()) {
        return QString();
    }

    // Links in HTML attributes escape their ampersands
    const QUrl pageUrl(url);
    const QUrl target = pageUrl.resolved(QUrl(link.replace(QStringLiteral("&amp;"), QStringLiteral("&"))));
    const QString scheme = target.scheme().toLower();
    const bool secure = (scheme == QStringLiteral("https")) || ((scheme == QStringLiteral("http")) && (pageUrl.scheme().toLower() == QStringLiteral("http")));
    if (!target.isValid() || target.host().isEmpty() || !secure) {
        if (errorString) {
            *errorString = tr("The page of the feed links its dataset at %1, which is not an https address.").arg(target.toString());
        }
        return QString();
    }
    return target.toString();
}

QDate GeoZoneProviders::airacDatasetDate(const QString &datasetUrl)
{
    if (datasetUrl.isEmpty()) {
        return QDate();
    }
    // The patterns match the links of the download pages
    const QString link = QStringLiteral("href=\"%1\"").arg(datasetUrl);
    for (const Service &service : _services()) {
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

bool GeoZoneProviders::isLiveUrl(const QString &url)
{
    const QList<Service> all = _services();
    const Service *const service = _serviceOf(all, url);
    return service && service->live;
}

double GeoZoneProviders::refreshHours(const QString &url)
{
    const QList<Service> all = _services();
    const Service *const service = _serviceOf(all, url);
    return service ? service->refreshHours : 0;
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
        const qint64 pageCount = (counts.matched - 1) / counts.returned;
        paging.pageCount = static_cast<int>((std::min)(pageCount, qint64{ maxWfsPages }));
        paging.truncated = pageCount > maxWfsPages;
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
        paging.truncated = (totalPages - page) > maxWfsPages;
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
