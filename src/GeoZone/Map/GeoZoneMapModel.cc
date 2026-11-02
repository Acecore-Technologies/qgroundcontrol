/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneMapModel.h"
#include "GeoZone.h"
#include "GeoZoneDisplay.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneManager.h"
#include "GeoZoneMissionReader.h"
#include "GeoZonePlanning.h"
#include "GeoZoneShape.h"
#include "GeoZoneSource.h"
#include "GeoZoneVehicleMonitor.h"
#include "MissionController.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "Vehicle.h"
#include "VisualMissionItem.h"

#include <QtCore/QRectF>
#include <QtCore/QSet>
#include <QtCore/QtMath>

#include <algorithm>
#include <limits>

GeoZoneMapModel::GeoZoneMapModel(QObject *parent)
    : GeoZoneMapModel(GeoZoneManager::instance(), parent)
{
}

GeoZoneMapModel::GeoZoneMapModel(GeoZoneManager *manager, QObject *parent)
    : QObject(parent)
    , _manager(manager ? manager : GeoZoneManager::instance())
{
    // Shapes are deleted when zones change, so the map layer must forget them first
    (void) connect(_manager, &GeoZoneManager::zonesAboutToChange, this, &GeoZoneMapModel::_removeSource);
    (void) connect(_manager, &GeoZoneManager::zonesChanged, this, &GeoZoneMapModel::_updateFilter);
    (void) connect(_manager, &GeoZoneManager::activeZonesChanged, this, &GeoZoneMapModel::_updateActive);
    (void) connect(_manager->display(), &GeoZoneDisplay::filtersChanged, this, &GeoZoneMapModel::_updateFilter);
    // The planned flight time of the ActiveAtFlightTime filter
    (void) connect(_manager->planning(), &GeoZonePlanning::plannedStartChanged, this, &GeoZoneMapModel::_updateFilter);
    (void) connect(_manager->planning(), &GeoZonePlanning::plannedEndChanged, this, &GeoZoneMapModel::_updateFilter);

    (void) connect(&_altitudeTimer, &QTimer::timeout, this, &GeoZoneMapModel::_updateAltitudes);
    _scheduleTimer.setInterval(60 * 1000);
    (void) connect(&_scheduleTimer, &QTimer::timeout, this, [this]() {
        _invalidateFilter();
        _refresh();
    });
    _missionLabelTimer.setSingleShot(true);
    _missionLabelTimer.setInterval(missionLabelDelayMSecs);
    (void) connect(&_missionLabelTimer, &QTimer::timeout, this, [this]() {
        if (_showLabels) {
            _refresh();
        }
    });
    _updateFilter();
}

GeoZoneMapModel::~GeoZoneMapModel()
{

}

void GeoZoneMapModel::setView(const QGeoRectangle &visibleRegion, double zoomLevel, bool showLabels)
{
    _visibleRegion = visibleRegion;
    _zoomLevel = zoomLevel;
    _showLabels = showLabels;

    if (!visibleRegion.isValid() || _manager->shapes().isEmpty()) {
        _clear();
        return;
    }

    // A view crossing the antimeridian is split in two
    const QPointF topLeft = GeoZoneShape::toMercator(visibleRegion.topLeft());
    const QPointF bottomRight = GeoZoneShape::toMercator(visibleRegion.bottomRight());
    QList<GeoZoneMercatorBounds> views;
    if (topLeft.x() <= bottomRight.x()) {
        views.append({ topLeft.x(), topLeft.y(), bottomRight.x(), bottomRight.y() });
    } else {
        views.append({ topLeft.x(), topLeft.y(), 1.0, bottomRight.y() });
        views.append({ 0.0, topLeft.y(), bottomRight.x(), bottomRight.y() });
    }

    double viewWidth = 0;
    for (const GeoZoneMercatorBounds &view : views) {
        viewWidth += view.width();
    }
    const double marginX = viewWidth * viewMargin;
    const double marginY = views.first().height() * viewMargin;
    QList<GeoZoneMercatorBounds> nearViews;
    QList<const GeoZoneShape*> nearShapes;
    QList<const GeoZoneShape*> filteredShapes;
    QSet<const GeoZoneShape*> nearShapeSet;
    for (const GeoZoneMercatorBounds &view : views) {
        const GeoZoneMercatorBounds nearView{ view.minX - marginX, view.minY - marginY, view.maxX + marginX, view.maxY + marginY };
        nearViews.append(nearView);

        // Mercator y grows southward, the edges of the projection include the poles
        const QGeoCoordinate northWest = GeoZoneShape::fromMercator(QPointF(std::clamp(nearView.minX, 0.0, 1.0), nearView.minY));
        const QGeoCoordinate southEast = GeoZoneShape::fromMercator(QPointF(std::clamp(nearView.maxX, 0.0, 1.0), nearView.maxY));
        const double north = (nearView.minY <= 0) ? 90.0 : northWest.latitude();
        const double south = (nearView.maxY >= 1) ? -90.0 : southEast.latitude();
        for (const GeoZoneShape *shape : _manager->shapesInArea(south, northWest.longitude(), north, southEast.longitude())) {
            if (!nearShapeSet.contains(shape)) {
                nearShapeSet.insert(shape);
                (isShown(shape->zone()) ? nearShapes : filteredShapes).append(shape);
            }
        }
    }

    const auto intersectsAny = [](const QList<GeoZoneMercatorBounds> &bounds, const GeoZoneMercatorBounds &shapeBounds) {
        return std::any_of(bounds.cbegin(), bounds.cend(), [&shapeBounds](const GeoZoneMercatorBounds &b) { return b.intersects(shapeBounds); });
    };

    QSet<const GeoZone*> filteredZones;
    for (const GeoZoneShape *shape : std::as_const(filteredShapes)) {
        if (intersectsAny(views, shape->mercatorBounds())) {
            filteredZones.insert(shape->zone());
        }
    }
    _setFilteredZoneCount(static_cast<int>(filteredZones.count()));

    struct Candidate {
        const GeoZoneShape *shape;
        double sizePixels;
        bool inView;
    };
    QList<Candidate> candidates;
    QSet<const GeoZone*> zonesInView;
    const double pixels = GeoZoneShape::mercatorPixels(zoomLevel);
    for (const GeoZoneShape *shape : std::as_const(nearShapes)) {
        const GeoZoneMercatorBounds &bounds = shape->mercatorBounds();
        if (!intersectsAny(nearViews, bounds)) {
            continue;
        }
        const bool inView = intersectsAny(views, bounds);
        if (inView) {
            zonesInView.insert(shape->zone());
        }
        candidates.append({ shape, (std::max)(bounds.width(), bounds.height()) * pixels, inView });
    }

    if (candidates.count() > maxShapeCount) {
        // Keep the shapes in view first, then the largest
        std::nth_element(candidates.begin(), candidates.begin() + maxShapeCount, candidates.end(), [](const Candidate &a, const Candidate &b) {
            return (a.inView != b.inView) ? a.inView : (a.sizePixels > b.sizePixels);
        });
        candidates.resize(maxShapeCount);
    }

    QSet<const GeoZoneShape*> shapes;
    QSet<const GeoZone*> shownZones;
    for (const Candidate &candidate : candidates) {
        shapes.insert(candidate.shape);
        shownZones.insert(candidate.shape->zone());
    }
    const int detailLevel = qFloor(zoomLevel);
    if ((shapes != _shapes) || (detailLevel != _detailLevel)) {
        _shapes = shapes;
        _detailLevel = detailLevel;
        emit shapesChanged();
    }

    const QVariantList labelInfos = GeoZoneInfo::toVariantList(showLabels ? _placeLabels(shownZones) : QList<const GeoZone*>());
    if (labelInfos != _labelZones) {
        _labelZones = labelInfos;
        emit labelZonesChanged();
    }

    int hiddenZoneCount = 0;
    for (const GeoZone *zone : std::as_const(zonesInView)) {
        if (!shownZones.contains(zone)) {
            hiddenZoneCount++;
        }
    }
    _setHiddenZoneCount(hiddenZoneCount);
}

QList<const GeoZone*> GeoZoneMapModel::_placeLabels(const QSet<const GeoZone*> &shownZones) const
{
    // Screen positions are approximated in web mercator from the north west corner of the view, ignoring its rotation
    const double pixels = GeoZoneShape::mercatorPixels(_zoomLevel);
    const QPointF origin = GeoZoneShape::toMercator(_visibleRegion.topLeft());
    const auto toScreen = [pixels, &origin](const QPointF &mercator) {
        // East of the antimeridian when the view crosses it
        const double x = (mercator.x() < origin.x()) ? (mercator.x() + 1) : mercator.x();
        return QPointF((x - origin.x()) * pixels, (mercator.y() - origin.y()) * pixels);
    };

    struct Candidate {
        const GeoZone *zone;
        int severity;
        double size;    ///< Pixels across the larger side of the zone's bounds
        QRectF rect;    ///< Estimated extent of the label
    };
    QList<Candidate> candidates;
    for (const GeoZone *zone : _manager->catalog().labelZones(_visibleRegion, std::numeric_limits<int>::max())) {
        if (!shownZones.contains(zone) ||
            ((zone->restriction() == GeoZone::NoRestriction) && !_highlightedZoneSet.contains(zone) && !_keptZoneSet.contains(zone))) {
            continue;
        }
        const GeoZoneBox &box = zone->box();
        const QPointF northWest = GeoZoneShape::toMercator(QPointF(box.west(), box.north()));
        const QPointF southEast = GeoZoneShape::toMercator(QPointF(box.east(), box.south()));
        const double size = (std::max)(southEast.x() - northWest.x(), southEast.y() - northWest.y()) * pixels;
        // The name, the restriction badge and the margins
        const double width = ((zone->displayName().length() + 1.5) * _labelCharWidth) + _labelHeight;
        const QPointF center = toScreen(GeoZoneShape::toMercator(zone->center()));
        candidates.append({ zone, GeoZone::restrictionSeverity(zone->restriction()), size,
                            QRectF(center.x() - (width / 2), center.y() - (_labelHeight / 2), width, _labelHeight) });
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
        if (a.severity != b.severity) {
            return a.severity < b.severity;
        }
        if (a.size != b.size) {
            return a.size > b.size;
        }
        return a.zone->key() < b.zone->key();
    });

    // Labels keep clear of the mission items, about twice their marker size
    QList<QRectF> placed;
    if (_missionController && _missionController->visualItems()) {
        const QmlObjectListModel *const items = _missionController->visualItems();
        const double itemSize = 2 * _labelHeight;
        for (int i = 0; i < items->count(); i++) {
            const VisualMissionItem *const item = items->value<const VisualMissionItem*>(i);
            if (item && item->specifiesCoordinate() && item->coordinate().isValid()) {
                const QPointF point = toScreen(GeoZoneShape::toMercator(item->coordinate()));
                placed.append(QRectF(point.x() - (itemSize / 2), point.y() - (itemSize / 2), itemSize, itemSize));
            }
        }
    }
    const qsizetype obstacleCount = placed.count();

    QList<const GeoZone*> zones;
    const double gap = _labelCharWidth / 2;
    for (const Candidate &candidate : std::as_const(candidates)) {
        const QRectF spaced = candidate.rect.adjusted(-gap, -gap, gap, gap);
        if (std::none_of(placed.cbegin(), placed.cend(), [&spaced](const QRectF &rect) { return rect.intersects(spaced); })) {
            placed.append(candidate.rect);
            zones.append(candidate.zone);
            if ((placed.count() - obstacleCount) >= GeoZoneManager::maxLabelCount) {
                break;
            }
        }
    }
    return zones;
}

void GeoZoneMapModel::setLabelCharWidth(qreal labelCharWidth)
{
    if (labelCharWidth != _labelCharWidth) {
        _labelCharWidth = labelCharWidth;
        emit labelMetricsChanged();
        _refresh();
    }
}

void GeoZoneMapModel::setLabelHeight(qreal labelHeight)
{
    if (labelHeight != _labelHeight) {
        _labelHeight = labelHeight;
        emit labelMetricsChanged();
        _refresh();
    }
}

void GeoZoneMapModel::_clear()
{
    if (!_labelZones.isEmpty()) {
        _labelZones.clear();
        emit labelZonesChanged();
    }
    if (!_shapes.isEmpty()) {
        _shapes.clear();
        emit shapesChanged();
    }
    _setHiddenZoneCount(0);
    _setFilteredZoneCount(0);
}

void GeoZoneMapModel::setHighlightedZones(const QVariantList &zones)
{
    if (zones == _highlightedZones) {
        return;
    }
    _highlightedZones = zones;
    const QList<const GeoZone*> highlighted = GeoZoneInfo::fromVariantList(zones);
    _highlightedZoneSet = QSet<const GeoZone*>(highlighted.cbegin(), highlighted.cend());
    emit shapesChanged();
    emit highlightedZonesChanged();
    // Highlighted zones are shown and labelled whatever the filters
    if (_filter.isActive() || _showLabels) {
        _refresh();
    }
}

void GeoZoneMapModel::setKeptZones(const QVariantList &zones)
{
    if (zones == _keptZones) {
        return;
    }
    _keptZones = zones;
    const QList<const GeoZone*> kept = GeoZoneInfo::fromVariantList(zones);
    _keptZoneSet = QSet<const GeoZone*>(kept.cbegin(), kept.cend());
    emit keptZonesChanged();
    if (_filter.isActive() || _showLabels) {
        _refresh();
    }
}

MissionController *GeoZoneMapModel::missionController() const
{
    return _missionController;
}

void GeoZoneMapModel::setMissionController(MissionController *missionController)
{
    if (missionController == _missionController) {
        return;
    }
    if (_missionController) {
        (void) disconnect(_missionController, nullptr, this, nullptr);
    }
    _missionController = missionController;
    if (_missionController) {
        const auto scheduleUpdate = [this]() {
            if (_filter.altitudes) {
                _altitudeTimer.start();
            }
        };
        (void) connect(_missionController, &MissionController::recalcTerrainProfile, this, scheduleUpdate);
        (void) connect(_missionController, &MissionController::visualItemsChanged, this, scheduleUpdate);
        (void) connect(_missionController, &MissionController::plannedHomePositionChanged, this, scheduleUpdate);
        // Labels move away from the items added, or where they were removed
        const auto placeLabels = [this]() { _missionLabelTimer.start(); };
        (void) connect(_missionController, &MissionController::visualItemsChanged, this, placeLabels);
        (void) connect(_missionController, &MissionController::recalcTerrainProfile, this, placeLabels);
    }
    _updateTimers();
    if (_filter.altitudes) {
        _updateAltitudes();
    }
    emit missionControllerChanged();
}

void GeoZoneMapModel::setVehicleAltitudes(bool vehicleAltitudes)
{
    if (vehicleAltitudes == _vehicleAltitudes) {
        return;
    }
    _vehicleAltitudes = vehicleAltitudes;
    _updateTimers();
    if (_filter.altitudes) {
        _updateAltitudes();
    }
    emit vehicleAltitudesChanged();
}

void GeoZoneMapModel::setAltitudeRange(const GeoZoneAltitudeRange &altitudeRange)
{
    if (altitudeRange.near(_altitudeRange, altitudeTolerance)) {
        return;
    }
    _altitudeRange = altitudeRange;
    if (_filter.altitudes) {
        _invalidateFilter();
        _refresh();
    }
}

bool GeoZoneMapModel::isShown(const GeoZone *zone)
{
    if (!_filter.isActive() || _keptZoneSet.contains(zone) || _highlightedZoneSet.contains(zone)) {
        return true;
    }
    auto it = _accepted.constFind(zone);
    if (it == _accepted.cend()) {
        it = _accepted.insert(zone, _filter.accepts(*zone, _filterTime, _altitudeRange));
    }
    return it.value();
}

QVariantList GeoZoneMapModel::zonesAt(const QGeoCoordinate &coordinate)
{
    QList<const GeoZone*> zones = _manager->catalog().zonesAt(coordinate);
    (void) zones.removeIf([this](const GeoZone *zone) { return !isShown(zone); });
    return GeoZoneInfo::toVariantList(zones);
}

void GeoZoneMapModel::_removeSource(GeoZoneSource *source)
{
    if (_shapes.removeIf([source](const GeoZoneShape *shape) { return shape->zone()->sourceKey() == source->key(); }) > 0) {
        emit shapesChanged();
    }
    _invalidateFilter();
    const qsizetype labelCount = _labelZones.count();
    _labelZones.removeIf([source](const QVariant &value) { return GeoZoneInfo::fromVariant(value)->sourceKey() == source->key(); });
    if (_labelZones.count() != labelCount) {
        emit labelZonesChanged();
    }
}

void GeoZoneMapModel::_updateActive()
{
    if (!_shapes.isEmpty()) {
        emit shapesChanged();
    }
    if (_filter.schedule != GeoZoneMapFilter::AnyTime) {
        _invalidateFilter();
        _refresh();
    }
}

void GeoZoneMapModel::_updateFilter()
{
    GeoZoneMapFilter filter = _manager->display()->filter();
    filter.flightStart = _manager->planning()->plannedStart();
    filter.flightEnd = _manager->planning()->plannedEnd();
    // The key of a source which was removed doesn't hide the zones of the others
    if (!filter.source.isEmpty() && !_manager->repository()->source(filter.source)) {
        filter.source.clear();
    }
    const bool readAltitudes = filter.altitudes && !_filter.altitudes;
    _filter = filter;
    _invalidateFilter();
    _updateTimers();
    if (readAltitudes && (_missionController || _vehicleAltitudes)) {
        _altitudeRange = _readAltitudes();
    }
    _refresh();
}

void GeoZoneMapModel::_invalidateFilter()
{
    _accepted.clear();
    _filterTime = GeoZoneEnvironment::currentDateTimeUtc();
}

void GeoZoneMapModel::_updateTimers()
{
    if (_filter.altitudes && _missionController) {
        _altitudeTimer.setSingleShot(true);
        _altitudeTimer.setInterval(missionAltitudeDelayMSecs);
    } else if (_filter.altitudes && _vehicleAltitudes) {
        _altitudeTimer.setSingleShot(false);
        _altitudeTimer.setInterval(vehicleAltitudeIntervalMSecs);
        if (!_altitudeTimer.isActive()) {
            _altitudeTimer.start();
        }
    } else {
        _altitudeTimer.stop();
    }

    if (_filter.schedule == GeoZoneMapFilter::ActiveSoon) {
        if (!_scheduleTimer.isActive()) {
            _scheduleTimer.start();
        }
    } else {
        _scheduleTimer.stop();
    }
}

GeoZoneAltitudeRange GeoZoneMapModel::_readAltitudes() const
{
    GeoZoneAltitudeRange range;
    if (_missionController) {
        const GeoZoneMissionPath path = GeoZoneMissionReader::path(_missionController);
        for (const QList<GeoZone::Position> &positions : path.positionLists()) {
            for (const GeoZone::Position &position : positions) {
                range.add(position);
            }
        }
        for (const GeoZone::Area &area : path.areas()) {
            range.add(area.lowest);
            range.add(area.highest);
        }
    } else if (_vehicleAltitudes) {
        Vehicle *const vehicle = MultiVehicleManager::instance()->activeVehicle();
        if (vehicle) {
            range.add(GeoZoneVehicleMonitor::vehiclePosition(vehicle, vehicle->coordinate()));
        }
    }
    return range;
}

void GeoZoneMapModel::_updateAltitudes()
{
    // Maps in hidden views skip the reads of the vehicle altitude
    if (_visibleRegion.isValid() || _missionController) {
        setAltitudeRange(_readAltitudes());
    }
}

void GeoZoneMapModel::_refresh()
{
    setView(_visibleRegion, _zoomLevel, _showLabels);
}

void GeoZoneMapModel::_setFilteredZoneCount(int filteredZoneCount)
{
    if (filteredZoneCount != _filteredZoneCount) {
        _filteredZoneCount = filteredZoneCount;
        emit filteredZoneCountChanged();
    }
}

void GeoZoneMapModel::_setHiddenZoneCount(int hiddenZoneCount)
{
    if (hiddenZoneCount != _hiddenZoneCount) {
        _hiddenZoneCount = hiddenZoneCount;
        emit hiddenZoneCountChanged(_hiddenZoneCount);
    }
}
