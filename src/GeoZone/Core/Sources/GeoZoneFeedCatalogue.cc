/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneFeedCatalogue.h"
#include "GeoZoneSource.h"

#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSet>
#include <QtCore/QUrl>

#include <algorithm>
#include <cmath>

namespace {

QString _string(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString().trimmed();
}

/// @return true for the addresses of a web server
bool _webUrl(const QString &url)
{
    // Templates are parsed with their placeholders, which QUrl accepts in tolerant mode
    const QUrl parsed(url);
    const QString scheme = parsed.scheme().toLower();
    return parsed.isValid() && ((scheme == QStringLiteral("https")) || (scheme == QStringLiteral("http"))) && !parsed.host().isEmpty();
}

/// @return true for the addresses feeds are downloaded or read from
bool _supportedUrl(const QString &url)
{
    const QUrl parsed(url);
    return _webUrl(url) || (parsed.isValid() && (parsed.scheme().toLower() == QStringLiteral("file")));
}

/// What a member of a catalogue must be: one of another type would be read as its default, like "enabled": "false"
/// as true
enum class MemberType : quint8 {
    Text,
    Boolean,
    Object,
    Other,      ///< Checked where it is read
};
using Members = QHash<QString, MemberType>;

const Members kRootMembers = {
    { QStringLiteral("fileType"), MemberType::Other }, { QStringLiteral("version"), MemberType::Other }, { QStringLiteral("feeds"), MemberType::Other },
    { QStringLiteral("services"), MemberType::Other },
    // Of the built-in catalogue, for its translations
    { QStringLiteral("translateKeys"), MemberType::Other }, { QStringLiteral("arrayIDKeys"), MemberType::Other },
};
const Members kFeedMembers = {
    { QStringLiteral("id"), MemberType::Text }, { QStringLiteral("name"), MemberType::Text }, { QStringLiteral("country"), MemberType::Text },
    { QStringLiteral("url"), MemberType::Text }, { QStringLiteral("areaUrl"), MemberType::Text }, { QStringLiteral("format"), MemberType::Text },
    { QStringLiteral("match"), MemberType::Object }, { QStringLiteral("datasetLink"), MemberType::Object }, { QStringLiteral("airac"), MemberType::Boolean },
    { QStringLiteral("refreshHours"), MemberType::Other }, { QStringLiteral("live"), MemberType::Boolean }, { QStringLiteral("licence"), MemberType::Text },
    { QStringLiteral("attribution"), MemberType::Text }, { QStringLiteral("info"), MemberType::Text }, { QStringLiteral("note"), MemberType::Text },
    { QStringLiteral("coverage"), MemberType::Other }, { QStringLiteral("serviceTemplate"), MemberType::Other }, { QStringLiteral("enabled"), MemberType::Boolean },
};
const Members kMatchMembers = { { QStringLiteral("host"), MemberType::Text }, { QStringLiteral("path"), MemberType::Text } };
const Members kDatasetLinkMembers = { { QStringLiteral("pattern"), MemberType::Text }, { QStringLiteral("dateFormat"), MemberType::Text } };
const Members kServiceTemplateMembers = { { QStringLiteral("maxAreaKm2"), MemberType::Other }, { QStringLiteral("note"), MemberType::Text } };
const Members kServiceMembers = {
    { QStringLiteral("id"), MemberType::Text }, { QStringLiteral("name"), MemberType::Text }, { QStringLiteral("nameFromData"), MemberType::Boolean },
    { QStringLiteral("url"), MemberType::Text }, { QStringLiteral("note"), MemberType::Text }, { QStringLiteral("format"), MemberType::Text },
    { QStringLiteral("supported"), MemberType::Boolean }, { QStringLiteral("service"), MemberType::Object }, { QStringLiteral("enabled"), MemberType::Boolean },
};

/// Checks the members of an object of a catalogue against those it may have
/// @param path Of the object in its entry, like "match", empty for the entry
/// @param unknown Appended with the members a catalogue doesn't have, like misspelled ones, which are ignored
/// @return What the first member of another type than expected must be, empty when all are as expected
QString _checkMembers(const QJsonObject &object, const Members &members, const QString &path, QStringList &unknown)
{
    QString problem;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const QString name = path.isEmpty() ? it.key() : (path + QLatin1Char('.') + it.key());
        const auto member = members.constFind(it.key());
        if (member == members.cend()) {
            unknown.append(name);
            continue;
        }
        if (!problem.isEmpty()) {
            continue;
        }
        switch (member.value()) {
        case MemberType::Text:
            if (!it.value().isString()) {
                problem = GeoZoneFeedCatalogue::tr("Its %1 must be a text.").arg(name);
            }
            break;
        case MemberType::Boolean:
            if (!it.value().isBool()) {
                problem = GeoZoneFeedCatalogue::tr("Its %1 must be true or false, without quotes.").arg(name);
            }
            break;
        case MemberType::Object:
            if (!it.value().isObject()) {
                problem = GeoZoneFeedCatalogue::tr("Its %1 must be an object.").arg(name);
            }
            break;
        case MemberType::Other:
            break;
        }
    }
    return problem;
}

} // namespace

GeoZoneParser::Format GeoZoneFeedCatalogue::formatFromName(const QString &name, bool &ok)
{
    using Format = GeoZoneParser::Format;
    static const QHash<QString, Format> formats = {
        { QString(), Format::Unknown }, { QStringLiteral("auto"), Format::Unknown },
        { QStringLiteral("ed269"), Format::ED269 }, { QStringLiteral("ed318"), Format::ED318 }, { QStringLiteral("aixm"), Format::Aixm },
        { QStringLiteral("dipul"), Format::Dipul }, { QStringLiteral("france-uas"), Format::FranceUas }, { QStringLiteral("faa-tfr"), Format::FaaTfr },
        { QStringLiteral("faa-airspace"), Format::FaaAirspace }, { QStringLiteral("openaip"), Format::OpenAip },
    };
    const auto it = formats.constFind(name.trimmed().toLower());
    ok = (it != formats.cend());
    return ok ? it.value() : Format::Unknown;
}

bool GeoZoneFeedCatalogue::parse(const QByteArray &json, const QString &origin, Catalogue &catalogue, QString &errorString)
{
    catalogue = Catalogue();
    errorString.clear();

    const QByteArray bytes = json.startsWith("\xEF\xBB\xBF") ? json.mid(3) : json;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        errorString = tr("Invalid JSON: %1 (offset %2).").arg(parseError.errorString()).arg(parseError.offset);
        return false;
    }
    const QJsonObject root = document.object();
    if (!document.isObject()) {
        errorString = tr("Not a feed catalogue: the document isn't a JSON object.");
        return false;
    }
    if (const QString type = _string(root, "fileType"); !type.isEmpty() && (type != QLatin1String(fileType))) {
        errorString = tr("Not a feed catalogue: its fileType is %1 rather than %2.").arg(type, QLatin1String(fileType));
        return false;
    }
    // Compared as the number it is: 2.5 or 1e10 are no integers, as which they would read as the default
    const QJsonValue versionValue = root.value(QStringLiteral("version"));
    const double versionNumber = versionValue.toDouble(version);
    if (!versionValue.isUndefined() && (!versionValue.isDouble() || (versionNumber < 1) || (versionNumber != std::floor(versionNumber)))) {
        errorString = tr("Its version must be a whole number, 1 for this format.");
        return false;
    }
    if (versionNumber > version) {
        errorString = tr("The catalogue has version %1, this QGroundControl reads version %2.").arg(QString::number(versionNumber, 'f', 0)).arg(version);
        return false;
    }
    const QJsonValue feedsValue = root.value(QStringLiteral("feeds"));
    const QJsonValue servicesValue = root.value(QStringLiteral("services"));
    if ((!feedsValue.isUndefined() && !feedsValue.isArray()) || (!servicesValue.isUndefined() && !servicesValue.isArray())) {
        errorString = tr("Its feeds and services must be lists.");
        return false;
    }
    if (feedsValue.isUndefined() && servicesValue.isUndefined()) {
        errorString = tr("Not a feed catalogue: it has no feeds or services.");
        return false;
    }

    // Members a catalogue doesn't have are told: a misspelled one would otherwise be ignored without a word
    QStringList unknown;
    (void) _checkMembers(root, kRootMembers, QString(), unknown);
    if (!unknown.isEmpty()) {
        catalogue.warnings.append(tr("The catalogue has unknown members, which are ignored: %1.").arg(unknown.join(QStringLiteral(", "))));
    }

    const bool translatable = origin.isEmpty();
    QSet<QString> ids;
    int index = 0;
    for (const QJsonValue &value : feedsValue.toArray()) {
        index++;
        const QJsonObject entry = value.toObject();
        const QString id = _string(entry, "id");
        if (id.isEmpty()) {
            catalogue.warnings.append(tr("Feed %1 has no id, it is left out.").arg(index));
            continue;
        }
        if (ids.contains(id)) {
            catalogue.warnings.append(tr("Feed %1 is listed twice, the first is used.").arg(id));
            continue;
        }
        ids.insert(id);
        unknown.clear();
        const QJsonValue serviceTemplate = entry.value(QStringLiteral("serviceTemplate"));
        QString problem = _checkMembers(entry, kFeedMembers, QString(), unknown);
        const auto checkMembersOf = [&entry, &unknown, &problem](const QString &member, const Members &members) {
            const QString memberProblem = _checkMembers(entry.value(member).toObject(), members, member, unknown);
            if (problem.isEmpty()) {
                problem = memberProblem;
            }
        };
        checkMembersOf(QStringLiteral("match"), kMatchMembers);
        checkMembersOf(QStringLiteral("datasetLink"), kDatasetLinkMembers);
        checkMembersOf(QStringLiteral("serviceTemplate"), kServiceTemplateMembers);
        if (problem.isEmpty() && !serviceTemplate.isUndefined() && !serviceTemplate.isObject() && !serviceTemplate.isBool()) {
            problem = tr("Its serviceTemplate must be an object, or true.");
        }
        if (!unknown.isEmpty()) {
            catalogue.warnings.append(tr("Feed %1 has unknown members, which are ignored: %2.").arg(id, unknown.join(QStringLiteral(", "))));
        }
        // A member which isn't read as written could turn a feed off or on against the intent: the entry changes nothing
        if (!problem.isEmpty()) {
            catalogue.warnings.append(tr("Feed %1: %2 It is left out.").arg(id, problem));
            continue;
        }
        if (!entry.value(QStringLiteral("enabled")).toBool(true)) {
            catalogue.disabled.append(id);
            continue;
        }

        GeoZoneProviders::Service service;
        service.id = id;
        service.origin = origin;
        service.translatable = translatable;
        service.name = _string(entry, "name");
        service.country = _string(entry, "country").toUpper();
        service.feedUrl = _string(entry, "url");
        service.areaUrlTemplate = _string(entry, "areaUrl");
        service.licence = _string(entry, "licence");
        service.attribution = _string(entry, "attribution");
        service.info = _string(entry, "info");
        service.note = _string(entry, "note");
        service.airac = entry.value(QStringLiteral("airac")).toBool();
        service.live = entry.value(QStringLiteral("live")).toBool();

        bool formatOk = true;
        service.format = formatFromName(_string(entry, "format"), formatOk);
        const QJsonValue refreshHours = entry.value(QStringLiteral("refreshHours"));
        const QJsonArray coverage = entry.value(QStringLiteral("coverage")).toArray();
        if (service.name.isEmpty()) {
            problem = tr("It has no name.");
        } else if (service.feedUrl.isEmpty() && service.areaUrlTemplate.isEmpty()) {
            problem = tr("It has neither a url nor an areaUrl.");
        } else if (!service.feedUrl.isEmpty() && !_supportedUrl(service.feedUrl)) {
            problem = tr("Its url isn't an https, http or file address.");
        } else if (!service.areaUrlTemplate.isEmpty() && (!_webUrl(service.areaUrlTemplate) || !GeoZoneService::hasAreaPlaceholders(service.areaUrlTemplate))) {
            problem = tr("Its areaUrl must be an https or http address with an area placeholder like {bbox}.");
        } else if (!formatOk) {
            problem = tr("Its format %1 is unknown.").arg(_string(entry, "format"));
        } else if (!refreshHours.isUndefined() && (!refreshHours.isDouble() || (refreshHours.toDouble() <= 0))) {
            problem = tr("Its refreshHours must be a number of hours.");
        } else if (entry.contains(QStringLiteral("coverage"))) {
            if ((coverage.count() != 4) || !std::all_of(coverage.cbegin(), coverage.cend(), [](const QJsonValue &bound) { return bound.isDouble(); })) {
                problem = tr("Its coverage must be [west, south, east, north] in degrees.");
            } else {
                service.coverage = QGeoRectangle(QGeoCoordinate(coverage[3].toDouble(), coverage[0].toDouble()), QGeoCoordinate(coverage[1].toDouble(), coverage[2].toDouble()));
                if (!service.coverage.isValid() || (coverage[3].toDouble() <= coverage[1].toDouble())) {
                    problem = tr("Its coverage must be [west, south, east, north] in degrees.");
                }
            }
        }
        service.refreshHours = refreshHours.toDouble(0);
        if (problem.isEmpty() && service.servesAreas() && !service.coverage.isValid()) {
            service.coverage = QGeoRectangle(QGeoCoordinate(90, -180), QGeoCoordinate(-90, 180));
        }

        const QJsonObject match = entry.value(QStringLiteral("match")).toObject();
        service.host = _string(match, "host");
        service.path = _string(match, "path");
        if (service.host.isEmpty()) {
            service.host = QUrl(service.feedUrl.isEmpty() ? service.areaUrlTemplate : service.feedUrl).host();
        }

        const QJsonObject datasetLink = entry.value(QStringLiteral("datasetLink")).toObject();
        if (problem.isEmpty() && !datasetLink.isEmpty()) {
            const QString pattern = _string(datasetLink, "pattern");
            service.datasetLinkPattern = GeoZoneProviders::linkPattern(pattern);
            if (const QString dateFormat = _string(datasetLink, "dateFormat"); !dateFormat.isEmpty()) {
                service.datasetDateFormat = dateFormat;
            }
            if (pattern.isEmpty() || !service.datasetLinkPattern.isValid()) {
                problem = tr("Its datasetLink pattern is invalid: %1.").arg(pattern.isEmpty() ? tr("empty") : service.datasetLinkPattern.errorString());
            } else if (service.datasetLinkPattern.captureCount() < 1) {
                problem = tr("Its datasetLink pattern doesn't capture the link.");
            }
        }

        if (problem.isEmpty() && (serviceTemplate.isObject() || serviceTemplate.toBool())) {
            if (!service.servesAreas()) {
                problem = tr("Only feeds with an areaUrl can be service templates.");
            }
            service.serviceTemplate = true;
            service.templateMaxAreaKm2 = (std::max)(0.0, serviceTemplate.toObject().value(QStringLiteral("maxAreaKm2")).toDouble());
            service.templateNote = _string(serviceTemplate.toObject(), "note");
        }

        if (!problem.isEmpty()) {
            catalogue.warnings.append(tr("Feed %1: %2 It is left out.").arg(id, problem));
            continue;
        }
        catalogue.feeds.append(service);
    }

    ids.clear();
    index = 0;
    for (const QJsonValue &value : servicesValue.toArray()) {
        index++;
        const QJsonObject entry = value.toObject();
        const QString id = _string(entry, "id");
        if (id.isEmpty()) {
            catalogue.warnings.append(tr("Service %1 has no id, it is left out.").arg(index));
            continue;
        }
        if (ids.contains(id)) {
            catalogue.warnings.append(tr("Service %1 is listed twice, the first is used.").arg(id));
            continue;
        }
        ids.insert(id);
        unknown.clear();
        QString problem = _checkMembers(entry, kServiceMembers, QString(), unknown);
        QStringList unknownOfService;
        const QStringList serviceProblems = GeoZoneService::jsonProblems(entry.value(QStringLiteral("service")).toObject(), &unknownOfService);
        for (const QString &member : std::as_const(unknownOfService)) {
            unknown.append(QStringLiteral("service.") + member);
        }
        if (problem.isEmpty() && !serviceProblems.isEmpty()) {
            problem = tr("Its service isn't read as written: %1.").arg(serviceProblems.join(QStringLiteral("; ")));
        }
        if (!unknown.isEmpty()) {
            catalogue.warnings.append(tr("Service %1 has unknown members, which are ignored: %2.").arg(id, unknown.join(QStringLiteral(", "))));
        }
        if (!problem.isEmpty()) {
            catalogue.warnings.append(tr("Service %1: %2 It is left out.").arg(id, problem));
            continue;
        }
        if (!entry.value(QStringLiteral("enabled")).toBool(true)) {
            catalogue.disabled.append(id);
            continue;
        }

        GeoZoneProviders::ServiceTemplate serviceTemplate;
        serviceTemplate.origin = origin;
        serviceTemplate.translatable = translatable;
        serviceTemplate.service = GeoZoneService::fromJson(entry.value(QStringLiteral("service")).toObject());
        serviceTemplate.service.id = id;
        const QString name = _string(entry, "name");
        // Without a name of their own, the sources of the service take the title of their data
        if (entry.value(QStringLiteral("nameFromData")).toBool()) {
            serviceTemplate.name = name;
            serviceTemplate.service.name.clear();
        } else {
            serviceTemplate.service.name = name;
        }
        serviceTemplate.url = _string(entry, "url");
        serviceTemplate.note = _string(entry, "note");
        serviceTemplate.supported = entry.value(QStringLiteral("supported")).toBool(true);
        bool formatOk = true;
        serviceTemplate.format = formatFromName(_string(entry, "format"), formatOk);

        if (name.isEmpty()) {
            problem = tr("It has no name.");
        } else if (!serviceTemplate.url.isEmpty() && !_supportedUrl(serviceTemplate.url)) {
            problem = tr("Its url isn't an https, http or file address.");
        } else if (!formatOk) {
            problem = tr("Its format %1 is unknown.").arg(_string(entry, "format"));
        }
        if (!problem.isEmpty()) {
            catalogue.warnings.append(tr("Service %1: %2 It is left out.").arg(id, problem));
            continue;
        }
        catalogue.templates.append(serviceTemplate);
    }
    return true;
}

GeoZoneFeedCatalogue::Catalogue GeoZoneFeedCatalogue::builtIn()
{
    Catalogue catalogue;
    QFile file(QString::fromLatin1(builtInPath));
    QString errorString;
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(GeoZoneSourceLog) << "The built-in feed catalogue can't be read:" << file.errorString();
    } else if (!parse(file.readAll(), QString(), catalogue, errorString)) {
        qCWarning(GeoZoneSourceLog) << "The built-in feed catalogue is invalid:" << errorString;
    }
    for (const QString &warning : std::as_const(catalogue.warnings)) {
        qCWarning(GeoZoneSourceLog) << "Built-in feed catalogue:" << warning;
    }
    return catalogue;
}

void GeoZoneFeedCatalogue::apply(Catalogue &catalogue, const Catalogue &overlay)
{
    for (const QString &id : overlay.disabled) {
        catalogue.feeds.removeIf([&id](const GeoZoneProviders::Service &service) { return service.id == id; });
        catalogue.templates.removeIf([&id](const GeoZoneProviders::ServiceTemplate &serviceTemplate) { return serviceTemplate.service.id == id; });
        if (!catalogue.disabled.contains(id)) {
            catalogue.disabled.append(id);
        }
    }
    for (const GeoZoneProviders::Service &service : overlay.feeds) {
        const auto it = std::find_if(catalogue.feeds.begin(), catalogue.feeds.end(), [&service](const GeoZoneProviders::Service &other) { return other.id == service.id; });
        if (it != catalogue.feeds.end()) {
            *it = service;
        } else {
            catalogue.feeds.append(service);
        }
        catalogue.disabled.removeAll(service.id);
    }
    for (const GeoZoneProviders::ServiceTemplate &serviceTemplate : overlay.templates) {
        const auto it = std::find_if(catalogue.templates.begin(), catalogue.templates.end(), [&serviceTemplate](const GeoZoneProviders::ServiceTemplate &other) {
            return other.service.id == serviceTemplate.service.id;
        });
        if (it != catalogue.templates.end()) {
            *it = serviceTemplate;
        } else {
            catalogue.templates.append(serviceTemplate);
        }
        catalogue.disabled.removeAll(serviceTemplate.service.id);
    }
}
