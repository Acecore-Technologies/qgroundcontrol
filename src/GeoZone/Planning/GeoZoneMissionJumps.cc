/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMissionJumps.h"

QHash<int, GeoZoneMissionJumps::Jump> GeoZoneMissionJumps::_jumps(const QList<GeoZoneMissionItem> &items)
{
    QHash<int, Jump> jumps;
    const int count = static_cast<int>(items.count());
    for (int i = 1; i < count; i++) {
        if (items[i].kind != GeoZoneMissionItem::Kind::Jump) {
            continue;
        }
        const int targetSequence = items[i].jumpSequence;
        for (int j = 1; j < count; j++) {
            if ((j != i) && (items[j].sequenceNumber <= targetSequence) && (targetSequence <= items[j].lastSequenceNumber)) {
                jumps.insert(i, Jump{ j, items[i].jumpRepeat });
                break;
            }
        }
    }
    return jumps;
}

bool GeoZoneMissionJumps::hasJumps(const QList<GeoZoneMissionItem> &items)
{
    return !_jumps(items).isEmpty();
}

GeoZoneMissionJumps::Walk GeoZoneMissionJumps::walk(const QList<GeoZoneMissionItem> &items, const QList<ItemSegments> &itemSegments,
                                                    const QList<double> &segmentDistances, int returnSegment, int maxSteps)
{
    const QHash<int, Jump> jumps = _jumps(items);
    const int count = static_cast<int>(items.count());

    // Walks the mission like the vehicle: on to the next item along the drawn segment, or straight to the item jumped to
    QHash<int, int> nextCoordinateItems;
    int previous = -1;
    for (int i = 0; i < count; i++) {
        if (items[i].flies) {
            if (previous >= 0) {
                nextCoordinateItems.insert(previous, i);
            }
            previous = i;
        }
    }
    Walk walk;
    const auto flyDrawn = [&walk, &segmentDistances](int segment) {
        const double distance = segmentDistances.value(segment);
        walk.steps.append(Step{ segment, -1, -1, QString(), distance });
        walk.distance += distance;
    };
    QHash<int, int> jumpsLeft;
    qsizetype jumpCount = 0;
    int lastCoordinateItem = -1;
    int jumpItem = -1;  ///< Jumped at, until the next item with a coordinate
    for (int i = 0; i < count; i++) {
        const GeoZoneMissionItem &item = items[i];
        if (const auto jump = jumps.constFind(i); jump != jumps.cend()) {
            if (!jumpsLeft.contains(i)) {
                // Loops without end are flown twice
                jumpsLeft.insert(i, (jump->repeat < 0) ? 1 : jump->repeat);
                walk.repeatsIndefinitely |= (jump->repeat < 0);
            }
            // Also limited for loops without segments
            if ((jumpsLeft[i] > 0) && (walk.steps.count() < maxSteps) && (jumpCount < maxSteps)) {
                jumpsLeft[i]--;
                jumpCount++;
                jumpItem = i;
                i = jump->target - 1;
            }
            continue;
        }
        if (item.kind == GeoZoneMissionItem::Kind::ReturnToLaunch) {
            break;
        }
        if (item.flies && (lastCoordinateItem >= 0)) {
            if ((jumpItem < 0) && (nextCoordinateItems.value(lastCoordinateItem, -1) == i)) {
                if (itemSegments[lastCoordinateItem].outgoing >= 0) {
                    flyDrawn(itemSegments[lastCoordinateItem].outgoing);
                }
            } else {
                const QGeoCoordinate from = items[lastCoordinateItem].exit.coordinate;
                const QGeoCoordinate to = item.entry.coordinate;
                QString label;
                if (jumpItem >= 0) {
                    const int jumpSequence = items[jumpItem].sequenceNumber;
                    label = (jumps.value(jumpItem).repeat < 0) ? tr("the jump at item %1, repeated without end").arg(jumpSequence) :
                                                                 tr("the jump at item %1").arg(jumpSequence);
                }
                if (from.isValid() && to.isValid() && (from.distanceTo(to) > 0.01)) {
                    const double distance = from.distanceTo(to);
                    walk.steps.append(Step{ -1, lastCoordinateItem, i, label, distance });
                    walk.distance += distance;
                }
            }
            jumpItem = -1;
        }
        if (!walk.itemDistances.contains(i)) {
            walk.itemDistances.insert(i, walk.distance);
        }
        for (const int segment : itemSegments[i].segments) {
            flyDrawn(segment);
        }
        if (item.flies) {
            lastCoordinateItem = i;
        }
    }
    if ((returnSegment >= 0) && (lastCoordinateItem >= 0) && (itemSegments[lastCoordinateItem].outgoing == returnSegment)) {
        flyDrawn(returnSegment);
    }
    return walk;
}
