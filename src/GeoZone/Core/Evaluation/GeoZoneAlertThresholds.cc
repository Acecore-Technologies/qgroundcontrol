/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneAlertThresholds.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLoggingCategory>
#include <QtCore/QtMath>

#include <algorithm>
#include <cmath>
#include <numbers>

Q_LOGGING_CATEGORY(GeoZoneAlertThresholdsLog, "qgc.geozone.geozonealertthresholds")

namespace {

/// Values of a profile in the JSON document, with the smallest value each accepts
struct Field {
    const char *name;
    double GeoZoneAlertThresholds::Profile::*member;
    double minimum;
    bool positive;      ///< The minimum itself is not accepted
};

const QList<Field> &fields()
{
    using Profile = GeoZoneAlertThresholds::Profile;
    static const QList<Field> list = {
        { "positionErrorFactor", &Profile::positionErrorFactor, 0, false },
        { "uere", &Profile::uere, 0, false },
        { "defaultPositionError", &Profile::defaultPositionError, 0, false },
        { "verticalErrorFactor", &Profile::verticalErrorFactor, 0, false },
        { "pilotReactionTime", &Profile::pilotReactionTime, 0, false },
        { "automaticReactionTime", &Profile::automaticReactionTime, 0, false },
        { "deceleration", &Profile::deceleration, 0, true },
        { "bankAngle", &Profile::bankAngle, 0, true },
        { "verticalDeceleration", &Profile::verticalDeceleration, 0, true },
        { "maxWind", &Profile::maxWind, 0, false },
        { "maxVerticalWind", &Profile::maxVerticalWind, 0, false },
        { "safetyMargin", &Profile::safetyMargin, 0, false },
        { "verticalSafetyMargin", &Profile::verticalSafetyMargin, 0, false },
        { "minDistance", &Profile::minDistance, 0, false },
        { "minVerticalDistance", &Profile::minVerticalDistance, 0, false },
    };
    return list;
}

/// Reads the values of a profile over those it starts with
bool readProfile(const QJsonObject &object, GeoZoneAlertThresholds::Profile &profile, QString &errorString)
{
    for (const Field &field : fields()) {
        const QJsonValue value = object.value(QLatin1String(field.name));
        if (value.isUndefined()) {
            continue;
        }
        const double number = value.toDouble(qQNaN());
        if (!value.isDouble() || !std::isfinite(number) || (number < field.minimum) || (field.positive && (number <= field.minimum))) {
            errorString = QStringLiteral("%1: invalid %2").arg(profile.vehicleClass, QLatin1String(field.name));
            return false;
        }
        profile.*field.member = number;
    }
    if (profile.bankAngle >= 89) {
        errorString = QStringLiteral("%1: invalid bankAngle").arg(profile.vehicleClass);
        return false;
    }
    const QString manoeuvre = object.value(QStringLiteral("manoeuvre")).toString();
    if (manoeuvre == QStringLiteral("brake")) {
        profile.manoeuvre = GeoZoneAlertThresholds::Manoeuvre::Brake;
    } else if (manoeuvre == QStringLiteral("turn")) {
        profile.manoeuvre = GeoZoneAlertThresholds::Manoeuvre::Turn;
    } else if (object.contains(QStringLiteral("manoeuvre"))) {
        errorString = QStringLiteral("%1: invalid manoeuvre").arg(profile.vehicleClass);
        return false;
    }
    if (object.contains(QStringLiteral("name"))) {
        profile.name = object.value(QStringLiteral("name")).toString();
    }
    return true;
}

QHash<QString, GeoZoneAlertThresholds::Profile> loadProfiles()
{
    for (const char *resource : { GeoZoneAlertThresholds::customProfilesResource, GeoZoneAlertThresholds::profilesResource }) {
        QFile file(QString::fromLatin1(resource));
        if (!file.exists()) {
            continue;
        }
        QString errorString;
        const QHash<QString, GeoZoneAlertThresholds::Profile> profiles = file.open(QIODevice::ReadOnly) ?
            GeoZoneAlertThresholds::parseProfiles(file.readAll(), &errorString) : QHash<QString, GeoZoneAlertThresholds::Profile>();
        if (!profiles.isEmpty()) {
            qCDebug(GeoZoneAlertThresholdsLog) << "Alert profiles of" << resource;
            return profiles;
        }
        qCWarning(GeoZoneAlertThresholdsLog) << "Unable to read the alert profiles of" << resource << errorString;
    }
    // The defaults of Profile
    GeoZoneAlertThresholds::Profile fallback;
    fallback.name = QStringLiteral("Default");
    return { { fallback.vehicleClass, fallback } };
}

} // namespace

GeoZoneAlertThresholds::Result GeoZoneAlertThresholds::compute(const Profile &profile, const Input &input, const Extras &extras, Reaction reaction)
{
    Result result;
    result.vehicleClass = profile.vehicleClass;
    result.profileName = profile.name;
    const double groundSpeed = input.velocity.valid ? input.velocity.groundSpeed() : 0;
    const double climb = input.velocity.valid ? std::abs(input.velocity.up) : 0;
    result.groundSpeed = groundSpeed;
    result.verticalSpeed = input.velocity.valid ? input.velocity.up : 0;

    // Position uncertainty: the accuracy the vehicle reports, or its dilution of precision times the range error
    double sigma = profile.defaultPositionError;
    if (input.horizontalAccuracy > 0) {
        sigma = input.horizontalAccuracy;
    } else if (input.hdop > 0) {
        sigma = input.hdop * profile.uere;
    }
    const double verticalSigma = (input.verticalAccuracy > 0) ? input.verticalAccuracy : (sigma * profile.verticalErrorFactor);
    result.positionError = profile.positionErrorFactor * sigma;
    result.verticalPositionError = profile.positionErrorFactor * verticalSigma;

    const double reactionTime = (reaction == Reaction::Pilot) ? profile.pilotReactionTime : profile.automaticReactionTime;
    result.reactionTime = reactionTime;

    // Manoeuvre span and time: braking to a stop, or turning away from the zone, whose closest point is a turn radius on
    double span = 0;
    double manoeuvreTime = 0;
    if (profile.manoeuvre == Manoeuvre::Turn) {
        const double lateral = gravity * std::tan(qDegreesToRadians(profile.bankAngle));
        span = (groundSpeed * groundSpeed) / lateral;
        manoeuvreTime = (std::numbers::pi / 2) * groundSpeed / lateral;
    } else {
        span = (groundSpeed * groundSpeed) / (2 * profile.deceleration);
        manoeuvreTime = groundSpeed / profile.deceleration;
    }
    const double verticalSpan = (climb * climb) / (2 * profile.verticalDeceleration);
    const double verticalManoeuvreTime = climb / profile.verticalDeceleration;
    result.manoeuvreSpan = span;
    result.manoeuvreTime = (std::max)(manoeuvreTime, verticalManoeuvreTime);

    // The wind drifts the vehicle while the pilot reacts and the vehicle manoeuvres
    result.windDrift = profile.maxWind * (reactionTime + manoeuvreTime);
    const double margin = result.positionError + result.windDrift + profile.safetyMargin;
    const double verticalMargin = result.verticalPositionError + (profile.maxVerticalWind * (reactionTime + verticalManoeuvreTime)) + profile.verticalSafetyMargin;

    result.extraTime = (std::max)(0.0, extras.time);
    result.extraDistance = (std::max)(0.0, extras.distance);
    const double ahead = reactionTime + evaluationInterval;
    result.distance = (std::max)(profile.minDistance, margin + (groundSpeed * ahead) + span) + result.extraDistance + (groundSpeed * result.extraTime);
    result.verticalDistance = (std::max)(profile.minVerticalDistance, verticalMargin + (climb * ahead) + verticalSpan) + (climb * result.extraTime);

    // The margins along the direction of travel, covered at its speed
    const double speed = std::hypot(groundSpeed, climb);
    const double marginAlong = (speed > 0) ? (std::hypot(margin * groundSpeed, verticalMargin * climb) / speed) : margin;
    const double time = reactionTime + result.manoeuvreTime + evaluationInterval + (marginAlong / (std::max)(speed, 1.0));
    result.time = (std::min)(time, maxTime) + result.extraTime;
    return result;
}

QHash<QString, GeoZoneAlertThresholds::Profile> GeoZoneAlertThresholds::parseProfiles(const QByteArray &json, QString *errorString)
{
    QString error;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    const QJsonObject classes = document.object().value(QStringLiteral("profiles")).toObject();
    QHash<QString, Profile> profiles;
    if (parseError.error != QJsonParseError::NoError) {
        error = parseError.errorString();
    } else if (document.object().value(QStringLiteral("fileType")).toString() != QStringLiteral("GeoZoneAlertProfiles")) {
        error = QStringLiteral("not a GeoZoneAlertProfiles file");
    } else {
        // The default class first, the others start from it
        Profile base;
        base.name = QStringLiteral("Default");
        if (readProfile(classes.value(QLatin1String(defaultClass)).toObject(), base, error)) {
            profiles.insert(base.vehicleClass, base);
            for (auto it = classes.constBegin(); (it != classes.constEnd()) && error.isEmpty(); ++it) {
                if (it.key() == QLatin1String(defaultClass)) {
                    continue;
                }
                Profile profile = base;
                profile.vehicleClass = it.key();
                profile.name = it.key();
                if (readProfile(it.value().toObject(), profile, error)) {
                    profiles.insert(profile.vehicleClass, profile);
                }
            }
        }
    }
    if (!error.isEmpty()) {
        profiles.clear();
    }
    if (errorString) {
        *errorString = error;
    }
    return profiles;
}

const QHash<QString, GeoZoneAlertThresholds::Profile> &GeoZoneAlertThresholds::profiles()
{
    static const QHash<QString, Profile> loaded = loadProfiles();
    return loaded;
}

GeoZoneAlertThresholds::Profile GeoZoneAlertThresholds::profile(const QString &vehicleClass)
{
    const QHash<QString, Profile> &all = profiles();
    const auto it = all.constFind(vehicleClass);
    return (it != all.cend()) ? *it : all.value(QString::fromLatin1(defaultClass));
}
