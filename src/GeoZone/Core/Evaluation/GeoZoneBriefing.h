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
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtPositioning/QGeoCoordinate>

#include <functional>
#include <optional>

#include "GeoZone.h"
#include "GeoZonePathEvaluator.h"
#include "GeoZoneSourceVersion.h"

class GeoZoneAuthorisations;
class GeoZoneCatalog;

/// Pre-flight briefing of the geozones of a mission: the zones its flight path enters, those only the returns from
/// along it would enter, and the restricted zones nearby, with where and when the mission meets them, their vertical
/// limits against the altitudes flown, their schedule during the flight, authority, messages and authorisation, and
/// the versions of the data. A plain value built from the evaluation of the paths, see GeoZonePathEvaluator, for the
/// exports of the application.
class GeoZoneBriefing
{
    Q_DECLARE_TR_FUNCTIONS(GeoZoneBriefing)

public:
    static constexpr double defaultNearbyDistance = 500;    ///< Meters from the flight path within which restricted zones are listed
    static constexpr int maxNearbySamples = 5000;           ///< Positions along the paths measured to the nearby zones
    static constexpr int maxScheduleSamples = 1500;         ///< Times the schedule of a zone is evaluated at during the flight

    /// How the mission meets a zone
    enum class Involvement {
        Entered,        ///< The flight path or a pattern area enters it, whether or not it applies then
        Contingency,    ///< Only returns from along the mission would enter it while it applies
        Nearby,         ///< A restricted zone within the nearby distance, or passed above or below
    };

    /// What the briefing is about, filled by the application
    struct Mission {
        QString name;               ///< Like the plan file name
        QString planFile;
        QString vehicle;            ///< Vehicle type
        QString firmware;
        QDateTime plannedStart;     ///< Invalid without a planned start
        QDateTime plannedEnd;
        double duration = qQNaN();          ///< Seconds
        double distance = qQNaN();          ///< Meters flown
        double maxAltitudeAMSL = qQNaN();   ///< Of the flight path
        double maxAltitudeAboveHome = qQNaN();
        QGeoCoordinate home;                ///< With the altitude AMSL when known
        int itemCount = 0;
        QStringList returnAssumptions;      ///< How the returns were modeled, like the return altitude
    };

    /// Mission item reached at a distance along the mission
    struct Item {
        int sequenceNumber = -1;
        double distance = 0;        ///< Meters flown when first reached
    };

    struct Interval {
        QDateTime start;
        QDateTime end;
        friend bool operator==(const Interval &, const Interval &) = default;
    };

    /// Stretch of a path inside a zone, see GeoZonePathEvaluator::Crossing
    struct Crossing {
        QString path;               ///< Label of the path, empty for the drawn flight path
        int entryItem = -1;         ///< Sequence number of the last item reached before the entry, -1 when unknown
        int exitItem = -1;
        double entryDistance = qQNaN(); ///< Meters along the mission
        double exitDistance = qQNaN();
        QGeoCoordinate entry;
        QGeoCoordinate exit;
        QDateTime entryTime;        ///< Estimated, invalid without a planned start
        QDateTime exitTime;
        double pathAltitudeMin = qQNaN();   ///< Meters AMSL flown inside the zone, NaN when unknown
        double pathAltitudeMax = qQNaN();
        double zoneFloor = qQNaN();         ///< Meters AMSL at the entry, NaN when the zone extends to the ground or the limit is unknown
        double zoneCeiling = qQNaN();       ///< NaN without ceiling or when unknown
        bool active = true;                 ///< See GeoZonePathEvaluator::Crossing::active
    };

    struct Authorisation {
        QString permit;
        QDateTime validFrom;
        QDateTime validTo;
        QString status;             ///< Like "Valid until 2026-12-31"
        bool coversFlight = false;  ///< Valid during the whole planned flight, or from now on without a planned start
    };

    struct Zone {
        Involvement involvement = Involvement::Entered;
        QString key;
        QString identifier;
        QString country;
        QString name;
        GeoZone::Restriction restriction = GeoZone::Unknown;
        QString restrictionCode;    ///< As published
        QString reasons;
        QString verticalLimits;     ///< As published
        QString schedule;           ///< As published, empty when permanent
        bool activeDuringFlight = true;     ///< Applies during the flight, see GeoZonePathEvaluator::ZoneResult::active
        QList<Interval> activePeriods;      ///< When it applies from the planned start to the end, widened by the schedule tolerance
        QList<Crossing> crossings;          ///< Of the flight paths, in mission order
        bool inArea = false;                ///< A pattern area overlaps the zone
        QStringList contingencies;          ///< Returns entering the zone with the items they start from, like "a return to launch from items 3–7"
        double distance = qQNaN();          ///< Meters horizontally from the flight path to nearby zones, 0 when passed above or below
        GeoZone::VerticalPosition passed = GeoZone::VerticalPosition::Inside;   ///< Above or Below for nearby zones passed over or under
        QStringList authorities;
        QString message;
        QString conditions;
        QString otherReasonInfo;
        QString uSpaceClass;
        QString regulationExemption;
        std::optional<Authorisation> authorisation;
        QString sourceKey;
        QByteArray contentHash;

        QString displayName() const { return name.isEmpty() ? identifier : name; }
    };

    /// Paths and data to brief
    struct Input {
        Mission mission;
        QList<GeoZonePathEvaluator::Path> paths;    ///< Flight paths with the estimated times, see GeoZoneMissionPath::evaluatorPaths
        QList<QList<double>> pathDistances;         ///< Meters along the mission at each vertex of the paths, from their start when missing
        QList<GeoZone::Area> areas;
        QList<GeoZonePathEvaluator::Path> contingencyPaths;
        /// Labels of the contingency paths entering each zone with the items they start from, the labels of the paths by
        /// default, see GeoZoneMissionCheck::groupContingencies
        std::function<QHash<const GeoZone*, QStringList>(const GeoZonePathEvaluator::Result &result)> groupContingencies;
        QList<Item> items;                          ///< By distance
        QDateTime start;                            ///< Of the flight, invalid to brief it at any time
        QDateTime end;
        int scheduleTolerance = GeoZonePathEvaluator::defaultScheduleToleranceSecs;
        double nearbyDistance = defaultNearbyDistance;
        const GeoZoneAuthorisations *authorisations = nullptr; ///< Looked up on the GUI thread
        QList<GeoZoneSourceVersion> sources;        ///< Of the data covering the mission
        QDateTime now;                              ///< Of the briefing, the current time when invalid
    };

    Mission mission;
    QList<Zone> zones;      ///< The entered ones in the order the mission enters them, then the contingency zones, most restrictive first, then the nearby ones, closest first
    QList<GeoZoneSourceVersion> sources;
    QDateTime generatedAt;  ///< UTC
    int scheduleTolerance = GeoZonePathEvaluator::defaultScheduleToleranceSecs;
    double nearbyDistance = defaultNearbyDistance;

    /// @return The briefing of the paths through the zones of the catalog
    static GeoZoneBriefing build(const GeoZoneCatalog &catalog, const Input &input);

    /// @return The zones with the involvement
    QList<const Zone*> zonesWith(Involvement involvement) const;

    /// @return Stable code of the involvement for exports: "entered", "contingency" or "nearby"
    static QString involvementCode(Involvement involvement);
    /// @return Stable code of the restriction for exports, like "prohibited" or "req_authorisation"
    static QString restrictionCode(GeoZone::Restriction restriction);

    /// @return Periods from start to end during which the zone applies, sampled every minute or more
    static QList<Interval> activePeriods(const GeoZone *zone, const QDateTime &start, const QDateTime &end);
};
