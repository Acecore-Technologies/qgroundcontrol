/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtPositioning/QGeoCoordinate>

#include <optional>

#include "GeoZoneAuthorisations.h"
#include "GeoZonePathEvaluator.h"

/// Operational intent of a planned flight, the request for a U-space or UTM flight authorisation: 4D volumes which
/// together contain the flight path with a horizontal and a vertical buffer while the vehicle is estimated to fly
/// it, widened by a time buffer. The volumes follow the ASTM F3548-21 data model of the USS interoperability API
/// (OperationalIntentDetails and Volume4D of utm.yaml, github.com/astm-utm/Protocol), which InterUSS uses and
/// U-space service providers accept. A plain value for the exports of the application, see toJson.
///
/// Each leg of the paths is buffered by the outline of a circle around its ends, adjacent legs in time are merged
/// while it doesn't add to the sum of their 4D extents, like the pieces of a straight leg, then the ones adding the
/// least until at most maxVolumes remain. Outlines are the convex hulls of the circles, so that they contain the
/// buffered legs whatever the merges. Altitudes are above the WGS84 ellipsoid, converted with the EGM96 geoid.
class GeoZoneOperationalIntent
{
    Q_DECLARE_TR_FUNCTIONS(GeoZoneOperationalIntent)

public:
    static constexpr const char *jsonSchema = "qgc-operational-intent";
    static constexpr int jsonVersion = 1;

    static constexpr double defaultHorizontalBuffer = 50;   ///< Meters
    static constexpr double defaultVerticalBuffer = 20;     ///< Meters
    static constexpr int defaultTimeBufferSecs = GeoZonePathEvaluator::defaultScheduleToleranceSecs;
    static constexpr int defaultMaxVolumes = 50;

    static constexpr int circleVertexCount = 16;    ///< Of the outline around each position, circumscribing the buffer
    /// Factor of the buffer of outlines and circles for the difference of the distances on the sphere of the
    /// construction and on the WGS84 ellipsoid of the standard, and for edges being geodesics there
    static constexpr double bufferMargin = 1.01;
    static constexpr double maxLegLength = 1000;    ///< Meters, longer legs are split along their great circle
    static constexpr double maxEdgeLength = 1000;   ///< Meters, longer outline edges are split so they stay near the geodesics

    struct Parameters {
        double horizontalBuffer = defaultHorizontalBuffer;  ///< Meters around the path
        double verticalBuffer = defaultVerticalBuffer;      ///< Meters below and above the path
        int timeBuffer = defaultTimeBufferSecs;             ///< Seconds before and after the estimated times
        int maxVolumes = defaultMaxVolumes;
    };

    /// Contiguous block of geographic spacetime, a Volume4D: a polygon, or a circle for a single position
    struct Volume {
        QList<QGeoCoordinate> outline;      ///< Unclosed polygon, counterclockwise, empty for a circle
        QGeoCoordinate circleCenter;        ///< Invalid for a polygon
        double circleRadius = 0;            ///< Meters
        double altitudeLower = qQNaN();     ///< Meters above the WGS84 ellipsoid
        double altitudeUpper = qQNaN();
        QDateTime timeStart;                ///< UTC, whole seconds
        QDateTime timeEnd;

        bool isCircle() const { return circleCenter.isValid(); }
        /// @return true if the coordinate is inside the outline, or the circle
        bool contains(const QGeoCoordinate &coordinate) const;
    };

    /// What the operational intent is for, filled by the application
    struct Flight {
        QString name;                       ///< Like the plan file name
        QString planFile;
        QString vehicle;
        QDateTime plannedStart;
        QDateTime plannedEnd;
        double distance = qQNaN();          ///< Meters flown
        bool repeatsIndefinitely = false;   ///< The volumes cover one pass of a mission repeating without end
    };

    /// U-space airspace the flight enters, where the operational intent needs an authorisation by its service providers
    struct Airspace {
        QString key;
        QString identifier;
        QString country;
        QString name;
    };

    Flight flight;
    Parameters parameters;
    QList<Volume> volumes;                          ///< By start time
    QList<Airspace> uSpaceAirspaces;
    QList<GeoZoneAuthorisation> authorisations;     ///< Held for zones the flight enters, their permits referencing the approvals
    QStringList notes;                              ///< Assumptions of the volumes, like the repeats of the mission
    int priority = 0;                               ///< Of the ASTM F3548-21 operational intent, as the regulator defines
    QDateTime generatedAt;                          ///< UTC

    /// @return The volumes containing the paths, which need times and altitudes, nothing on errors with errorString set
    static std::optional<QList<Volume>> buildVolumes(const QList<GeoZonePathEvaluator::Path> &paths, const Parameters &parameters,
                                                     QString &errorString);

    /// @return Start of the first volume, invalid without volumes
    QDateTime timeStart() const;
    /// @return End of the last volume, invalid without volumes
    QDateTime timeEnd() const;

    /// @return The document of the export: the description of the flight with "schema" jsonSchema and "version"
    ///         jsonVersion, and "operational_intent" with the ASTM F3548-21 "details" and the "reference" fields known
    ///         before a USS submits it: "state", "time_start" and "time_end"
    QJsonObject toJson() const;
    /// @return The ASTM F3548-21 OperationalIntentDetails of the volumes
    QJsonObject detailsJson() const;
    /// @return The ASTM F3548-21 Volume4D of the volume
    static QJsonObject volumeJson(const Volume &volume);
    /// @return The ASTM F3548-21 Time, RFC 3339 in UTC
    static QJsonObject timeJson(const QDateTime &time);
};
