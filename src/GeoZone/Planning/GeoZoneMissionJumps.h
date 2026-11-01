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
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QString>

#include "GeoZoneMissionItem.h"

/// The order a vehicle flies the items of a mission with DO_JUMP items: on to the next item along the drawn flight
/// path, or straight back to the item a DO_JUMP jumps to as many times as its repeat count, a jump repeating without
/// end being flown once more. A pure computation on the items and the drawn segments, see GeoZoneMissionReader.
class GeoZoneMissionJumps
{
    Q_DECLARE_TR_FUNCTIONS(GeoZoneMissionJumps)

public:
    /// Drawn flight path of an item, by indexes into the drawn segments
    struct ItemSegments {
        QList<int> segments;    ///< Of the item itself, like the flight lines of a pattern
        int outgoing = -1;      ///< To the next item of the drawn flight path, -1 without
    };

    /// Part of the flight
    struct Step {
        int segment = -1;       ///< Drawn segment flown, -1 for a straight leg between items which isn't drawn
        int fromItem = -1;      ///< Of a straight leg, from the exit of the item
        int toItem = -1;        ///< Of a straight leg, to the entry of the item
        QString label;          ///< Of a straight leg, like "the jump at item 5"
        double distance = 0;    ///< Meters
    };

    /// Flight along the items
    struct Walk {
        QList<Step> steps;
        QHash<int, double> itemDistances;   ///< Meters flown when the vehicle first reaches the items, by index
        double distance = 0;                ///< Meters flown
        bool repeatsIndefinitely = false;   ///< A DO_JUMP repeats without end, flown once more
    };

    /// @return true if a DO_JUMP of the items jumps to an item of the mission
    static bool hasJumps(const QList<GeoZoneMissionItem> &items);

    /// @return The flight along the items, until the end of the mission or a return to launch, then home along the
    ///         drawn return segment. The repeats stop once maxSteps steps are flown.
    /// @param items The visual items of the mission, the planned home first
    /// @param itemSegments Drawn segments of the items, by item index
    /// @param segmentDistances Meters along the drawn segments
    /// @param returnSegment Drawn segment home after a return to launch, -1 without
    static Walk walk(const QList<GeoZoneMissionItem> &items, const QList<ItemSegments> &itemSegments, const QList<double> &segmentDistances,
                     int returnSegment, int maxSteps);

private:
    /// DO_JUMP with the index of the item it jumps to and how many times
    struct Jump {
        int target = -1;
        int repeat = 0;     ///< Negative without end
    };

    /// @return The DO_JUMP items by index
    static QHash<int, Jump> _jumps(const QList<GeoZoneMissionItem> &items);
};
