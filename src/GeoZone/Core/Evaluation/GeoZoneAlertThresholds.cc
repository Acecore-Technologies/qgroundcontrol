/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneAlertThresholds.h"

#include <QtCore/QCoreApplication>
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
        { "pilotReactionTime", &Profile::pilotReactionTime, 0, true },
        { "automaticReactionTime", &Profile::automaticReactionTime, 0, true },
        { "deceleration", &Profile::deceleration, 0, true },
        { "bankAngle", &Profile::bankAngle, 0, true },
        { "verticalDeceleration", &Profile::verticalDeceleration, 0, true },
        { "maxWind", &Profile::maxWind, 0, false },
        { "maxVerticalWind", &Profile::maxVerticalWind, 0, false },
        { "safetyMargin", &Profile::safetyMargin, 0, false },
        { "verticalSafetyMargin", &Profile::verticalSafetyMargin, 0, false },
        { "minDistance", &Profile::minDistance, 0, false },
        { "minVerticalDistance", &Profile::minVerticalDistance, 0, false },
        { "fenceWarningMargin", &Profile::fenceWarningMargin, 0, false },
        { "assumedSpeed", &Profile::assumedSpeed, 0, true },
        { "assumedVerticalSpeed", &Profile::assumedVerticalSpeed, 0, true },
        { "minSatellites", &Profile::minSatellites, 0, false },
        { "maxHdop", &Profile::maxHdop, 0, true },
        { "maxHorizontalAccuracy", &Profile::maxHorizontalAccuracy, 0, true },
    };
    return list;
}

/// Reads the values of a profile over those it starts with
/// @param warnings Gets the keys which aren't values of a profile, like misspelt ones, whose values stay as they were
bool readProfile(const QJsonObject &object, GeoZoneAlertThresholds::Profile &profile, QString &errorString, QStringList &warnings)
{
    static const QStringList otherKeys = { QStringLiteral("name"), QStringLiteral("manoeuvre"), QStringLiteral("description") };
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const QString &key = it.key();
        const bool known = otherKeys.contains(key) ||
                           std::any_of(fields().cbegin(), fields().cend(), [&key](const Field &field) { return key == QLatin1String(field.name); });
        if (!known) {
            warnings.append(QStringLiteral("%1: unknown value %2").arg(profile.vehicleClass, key));
        }
    }
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
    // The geofence warning comes this long before the breach, and never needs none
    if ((profile.pilotReactionTime + profile.fenceWarningMargin) <= 0) {
        errorString = QStringLiteral("%1: invalid fenceWarningMargin").arg(profile.vehicleClass);
        return false;
    }
    return true;
}

/// The profiles in use, with what the pilot is told about them
struct Loaded {
    QHash<QString, GeoZoneAlertThresholds::Profile> profiles;
    QString warning;
};

const Loaded &loaded()
{
    static const Loaded result = []() {
        Loaded loading;
        loading.profiles = GeoZoneAlertThresholds::loadProfiles({ QString::fromLatin1(GeoZoneAlertThresholds::customProfilesResource),
                                                                  QString::fromLatin1(GeoZoneAlertThresholds::profilesResource) }, &loading.warning);
        return loading;
    }();
    return result;
}

} // namespace

GeoZoneAlertThresholds::Result GeoZoneAlertThresholds::compute(const Profile &profile, const Input &input, const Extras &extras, Reaction reaction)
{
    Result result;
    result.vehicleClass = profile.vehicleClass;
    result.profileName = profile.name;
    // Without a velocity, the speeds the vehicle reports or those assumed for its class
    double groundSpeed = 0;
    double climb = 0;
    result.speedSource = input.velocity.valid ? SpeedSource::Velocity : input.speedSource;
    switch (result.speedSource) {
    case SpeedSource::Velocity:
        groundSpeed = input.velocity.groundSpeed();
        climb = std::abs(input.velocity.up);
        break;
    case SpeedSource::Reported:
        groundSpeed = std::isfinite(input.groundSpeed) ? std::abs(input.groundSpeed) : profile.assumedSpeed;
        climb = std::isfinite(input.verticalSpeed) ? std::abs(input.verticalSpeed) : profile.assumedVerticalSpeed;
        break;
    case SpeedSource::Assumed:
        groundSpeed = profile.assumedSpeed;
        climb = profile.assumedVerticalSpeed;
        break;
    case SpeedSource::Rest:
        break;
    }
    result.groundSpeed = groundSpeed;
    result.verticalSpeed = input.velocity.valid ? input.velocity.up : climb;

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

QHash<QString, GeoZoneAlertThresholds::Profile> GeoZoneAlertThresholds::parseProfiles(const QByteArray &json, QString *errorString, QStringList *warnings)
{
    QString error;
    QStringList ignored;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    const QJsonObject root = document.object();
    const QJsonValue version = root.value(QStringLiteral("version"));
    const QJsonValue classesValue = root.value(QStringLiteral("profiles"));
    const QJsonObject classes = classesValue.toObject();
    QHash<QString, Profile> profiles;
    if (parseError.error != QJsonParseError::NoError) {
        error = parseError.errorString();
    } else if (root.value(QStringLiteral("fileType")).toString() != QStringLiteral("GeoZoneAlertProfiles")) {
        error = QStringLiteral("not a GeoZoneAlertProfiles file");
    } else if (!version.isDouble() || (version.toDouble() != profilesVersion)) {
        // A newer format may mean its values differently
        error = version.isUndefined() ? QStringLiteral("no version") : QStringLiteral("unsupported version %1").arg(version.toVariant().toString());
    } else if (!classesValue.isObject() || classes.isEmpty()) {
        error = QStringLiteral("no profiles");
    } else {
        static const QStringList rootKeys = { QStringLiteral("fileType"), QStringLiteral("version"), QStringLiteral("description"), QStringLiteral("profiles") };
        for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
            if (!rootKeys.contains(it.key())) {
                ignored.append(QStringLiteral("unknown value %1").arg(it.key()));
            }
        }
        for (auto it = classes.constBegin(); (it != classes.constEnd()) && error.isEmpty(); ++it) {
            if (!it.value().isObject()) {
                error = QStringLiteral("%1: not a profile").arg(it.key());
            }
        }
        // The default class first, the others start from it
        Profile base;
        base.name = QStringLiteral("Default");
        if (error.isEmpty() && readProfile(classes.value(QLatin1String(defaultClass)).toObject(), base, error, ignored)) {
            profiles.insert(base.vehicleClass, base);
            for (auto it = classes.constBegin(); (it != classes.constEnd()) && error.isEmpty(); ++it) {
                if (it.key() == QLatin1String(defaultClass)) {
                    continue;
                }
                Profile profile = base;
                profile.vehicleClass = it.key();
                profile.name = it.key();
                if (readProfile(it.value().toObject(), profile, error, ignored)) {
                    profiles.insert(profile.vehicleClass, profile);
                }
            }
        }
    }
    if (!error.isEmpty()) {
        profiles.clear();
        ignored.clear();
    }
    if (errorString) {
        *errorString = error;
    }
    if (warnings) {
        *warnings = ignored;
    }
    return profiles;
}

QHash<QString, GeoZoneAlertThresholds::Profile> GeoZoneAlertThresholds::loadProfiles(const QStringList &fileNames, QString *warning)
{
    QStringList told;
    QHash<QString, Profile> profiles;
    for (const QString &fileName : fileNames) {
        QFile file(fileName);
        if (!file.exists()) {
            continue;
        }
        QString errorString;
        QStringList ignored;
        if (file.open(QIODevice::ReadOnly)) {
            profiles = parseProfiles(file.readAll(), &errorString, &ignored);
        } else {
            errorString = file.errorString();
        }
        if (!profiles.isEmpty()) {
            qCDebug(GeoZoneAlertThresholdsLog) << "Alert profiles of" << fileName;
            if (!ignored.isEmpty()) {
                // A misspelt value leaves the default one in use, which the manufacturer didn't mean
                qCWarning(GeoZoneAlertThresholdsLog) << "Alert profiles of" << fileName << "ignored:" << ignored;
                told.append(QCoreApplication::translate("GeoZoneAlertThresholds", "Alert profiles %1: %2. The default values are used for them.")
                                .arg(fileName, ignored.join(QStringLiteral(", "))));
            }
            break;
        }
        qCWarning(GeoZoneAlertThresholdsLog) << "Unable to read the alert profiles of" << fileName << errorString;
        told.append(QCoreApplication::translate("GeoZoneAlertThresholds", "The alert profiles %1 can't be used (%2). The default profiles are used instead.")
                        .arg(fileName, errorString));
    }
    if (profiles.isEmpty()) {
        // The defaults of Profile
        Profile fallback;
        fallback.name = QStringLiteral("Default");
        profiles.insert(fallback.vehicleClass, fallback);
    }
    if (warning) {
        *warning = told.join(QLatin1Char(' '));
    }
    return profiles;
}

const QHash<QString, GeoZoneAlertThresholds::Profile> &GeoZoneAlertThresholds::profiles()
{
    return loaded().profiles;
}

QString GeoZoneAlertThresholds::profilesWarning()
{
    return loaded().warning;
}

GeoZoneAlertThresholds::Profile GeoZoneAlertThresholds::profile(const QString &vehicleClass)
{
    const QHash<QString, Profile> &all = profiles();
    const auto it = all.constFind(vehicleClass);
    return (it != all.cend()) ? *it : all.value(QString::fromLatin1(defaultClass));
}
