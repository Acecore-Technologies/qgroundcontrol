/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneOperatorZones.h"
#include "GeoZoneEd318Writer.h"
#include "GeoZoneEnvironment.h"
#include "GeoZoneParser.h"
#include "GeoZoneSource.h"
#include "GeoZoneSourceRepository.h"
#include "GeoZoneText.h"
#include "GeoZoneVertical.h"
#include "QGCMapCircle.h"
#include "QGCMapPolygon.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QUuid>

#include <cmath>

namespace {

double _meters(double limit, bool feet)
{
    return feet ? (limit * GeoZoneVertical::metersPerFoot) : limit;
}

double _number(const QVariant &value)
{
    bool ok = false;
    const double number = value.toDouble(&ok);
    return (ok && std::isfinite(number)) ? number : qQNaN();
}

} // namespace

GeoZoneOperatorZones::GeoZoneOperatorZones(GeoZoneSourceRepository *repository, QObject *parent)
    : QObject(parent)
    , _repository(repository)
    , _polygon(new QGCMapPolygon(this))
    , _circle(new QGCMapCircle(this))
{
    // Emitted when a source was removed or its zones were replaced
    (void) connect(_repository, &GeoZoneSourceRepository::sourceChanged, this, &GeoZoneOperatorZones::_sourceChanged);
    load();
}

GeoZoneOperatorZones::~GeoZoneOperatorZones()
{
}

QString GeoZoneOperatorZones::fileName() const
{
    // In a subdirectory, which the store doesn't clean up as one of its copies
    return QDir(_repository->storageDir()).filePath(QStringLiteral("operator/my-zones.json"));
}

void GeoZoneOperatorZones::load()
{
    const QString path = fileName();
    QList<GeoZone::Data> zones;
    QString loadError;
    QFile file(path);
    // No file, or one emptied instead of deleted, is no zones
    if (file.exists() && (file.size() > 0)) {
        GeoZoneParser::Result result;
        QString errorString;
        if (!file.open(QIODevice::ReadOnly)) {
            errorString = file.errorString();
        } else {
            const QByteArray document = file.readAll();
            if (file.error() != QFileDevice::NoError) {
                errorString = file.errorString();
            } else if (GeoZoneParser::parse(document, result, errorString)) {
                zones = result.zones;
            } else if (errorString.isEmpty()) {
                errorString = tr("it is not a geozone file");
            }
        }
        if (!errorString.isEmpty()) {
            loadError = tr("The zones you drew can't be read from %1: %2. They aren't shown or checked. The file is kept, new zones are saved beside it.")
                            .arg(path, errorString);
        }
    }
    const bool changed = !_zones.isEmpty() || !zones.isEmpty();
    _zones = zones;
    if (changed) {
        emit zonesChanged();
    }
    _setLoadError(loadError);
}

void GeoZoneOperatorZones::restoreSource()
{
    if (!_zones.isEmpty() && !_repository->hasSource(fileName())) {
        _repository->restoreFile(fileName());
    }
}

void GeoZoneOperatorZones::_setLoadError(const QString &loadError)
{
    if (loadError != _loadError) {
        _loadError = loadError;
        emit loadErrorChanged();
    }
}

bool GeoZoneOperatorZones::_removeFile(QString &errorString)
{
    QFile file(fileName());
    if (!file.exists() || file.remove()) {
        return true;
    }
    // An empty file has no zones either, see load
    const QString removeError = file.errorString();
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return true;
    }
    errorString = tr("The zones can't be removed from %1: %2").arg(file.fileName(), removeError);
    return false;
}

bool GeoZoneOperatorZones::_setUnreadableFileAside(QString &errorString)
{
    const QString path = fileName();
    if (!QFile::exists(path)) {
        return true;
    }
    const QFileInfo info(path);
    const QString stamp = GeoZoneEnvironment::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    for (int attempt = 0; attempt < 100; attempt++) {
        const QString suffix = (attempt == 0) ? stamp : QStringLiteral("%1-%2").arg(stamp).arg(attempt);
        const QString aside = info.dir().filePath(QStringLiteral("%1.unreadable-%2.%3").arg(info.completeBaseName(), suffix, info.suffix()));
        if (!QFile::exists(aside)) {
            if (QFile::rename(path, aside)) {
                return true;
            }
            break;
        }
    }
    errorString = tr("The zones can't be saved: the file %1, which couldn't be read, can't be moved out of the way.").arg(path);
    return false;
}

QVariantList GeoZoneOperatorZones::zones() const
{
    QVariantList list;
    for (const GeoZone::Data &zone : _zones) {
        list.append(QVariantMap{
            { QStringLiteral("name"), zone.name },
            { QStringLiteral("restriction"), static_cast<int>(zone.restriction) },
            { QStringLiteral("restrictionText"), GeoZoneText::restriction(zone.restriction) },
            { QStringLiteral("circle"), !zone.volumes.isEmpty() && zone.volumes.first().isCircle },
        });
    }
    return list;
}

QStringList GeoZoneOperatorZones::restrictionNames()
{
    return { GeoZoneText::restriction(GeoZone::Prohibited), GeoZoneText::restriction(GeoZone::ReqAuthorisation),
             GeoZoneText::restriction(GeoZone::Conditional), GeoZoneText::restriction(GeoZone::NoRestriction) };
}

void GeoZoneOperatorZones::_setEditing(bool editing, int index, bool circle, const QVariantMap &properties)
{
    _editing = editing;
    _editIndex = index;
    _editCircle = circle;
    _editProperties = properties;
    _polygon->setInteractive(editing && !circle);
    _circle->setInteractive(editing && circle);
    if (!editing) {
        _polygon->clear();
    }
    emit editingChanged();
}

void GeoZoneOperatorZones::startPolygon(const QGeoRectangle &view)
{
    if (!view.isValid()) {
        return;
    }
    // A square of a third of the view
    const QGeoCoordinate center = view.center();
    const double halfSide = (std::min)(center.distanceTo(QGeoCoordinate(center.latitude(), view.topLeft().longitude())),
                                       center.distanceTo(QGeoCoordinate(view.topLeft().latitude(), center.longitude()))) / 3;
    const double halfDiagonal = halfSide * std::sqrt(2.0);
    _polygon->clear();
    _polygon->setPath(QList<QGeoCoordinate>{ center.atDistanceAndAzimuth(halfDiagonal, 315), center.atDistanceAndAzimuth(halfDiagonal, 45),
                                             center.atDistanceAndAzimuth(halfDiagonal, 135), center.atDistanceAndAzimuth(halfDiagonal, 225) });
    GeoZone::Data zone;
    zone.name = tr("My zone %1").arg(_zones.count() + 1);
    zone.restriction = GeoZone::Prohibited;
    _setEditing(true, -1, false, properties(zone));
}

void GeoZoneOperatorZones::startCircle(const QGeoRectangle &view)
{
    if (!view.isValid()) {
        return;
    }
    const QGeoCoordinate center = view.center();
    _circle->setCenter(center);
    _circle->radius()->setRawValue((std::min)(center.distanceTo(QGeoCoordinate(center.latitude(), view.topLeft().longitude())),
                                              center.distanceTo(QGeoCoordinate(view.topLeft().latitude(), center.longitude()))) / 3);
    GeoZone::Data zone;
    zone.name = tr("My zone %1").arg(_zones.count() + 1);
    zone.restriction = GeoZone::Prohibited;
    _setEditing(true, -1, true, properties(zone));
}

void GeoZoneOperatorZones::startEdit(int index)
{
    if ((index < 0) || (index >= _zones.count()) || _zones[index].volumes.isEmpty()) {
        return;
    }
    const GeoZone::Volume &volume = _zones[index].volumes.first();
    if (volume.isCircle) {
        _circle->setCenter(volume.center);
        _circle->radius()->setRawValue(volume.radius);
    } else {
        _polygon->clear();
        _polygon->setPath(GeoZone::toCoordinates(volume.rings.value(0)));
    }
    _setEditing(true, index, volume.isCircle, properties(_zones[index]));
}

void GeoZoneOperatorZones::cancelEdit()
{
    if (_editing) {
        _setEditing(false, -1, false, QVariantMap());
    }
}

bool GeoZoneOperatorZones::saveEdit(const QVariantMap &properties)
{
    if (!_editing) {
        return false;
    }
    GeoZone::Volume shape;
    if (_editCircle) {
        shape.isCircle = true;
        shape.center = _circle->center();
        shape.radius = _circle->radius()->rawValue().toDouble();
    } else {
        const QList<QGeoCoordinate> path = _polygon->coordinateList();
        if (path.count() < 3) {
            emit failed(tr("The zone needs at least 3 points."));
            return false;
        }
        shape.rings.append(GeoZone::toRing(path));
    }

    QList<GeoZone::Data> zones = _zones;
    const bool existing = (_editIndex >= 0) && (_editIndex < zones.count());
    const QString identifier = existing ? zones[_editIndex].identifier : (QStringLiteral("MY-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8).toUpper());
    const GeoZone::Data zone = zoneFromProperties(properties, shape, identifier);
    if (existing) {
        zones[_editIndex] = zone;
    } else {
        zones.append(zone);
    }
    QString errorString;
    if (!setZones(zones, errorString)) {
        emit failed(errorString);
        return false;
    }
    _setEditing(false, -1, false, QVariantMap());
    return true;
}

bool GeoZoneOperatorZones::removeZone(int index)
{
    if ((index < 0) || (index >= _zones.count())) {
        return false;
    }
    QList<GeoZone::Data> zones = _zones;
    zones.removeAt(index);
    QString errorString;
    if (!setZones(zones, errorString)) {
        emit failed(errorString);
        return false;
    }
    return true;
}

bool GeoZoneOperatorZones::setZones(const QList<GeoZone::Data> &zones, QString &errorString)
{
    const QString path = fileName();
    if (zones.isEmpty()) {
        // The file first: zones left in it would be listed again at the next start, without their source. A file which
        // couldn't be read has none of the zones to remove, and is kept.
        if (_loadError.isEmpty() && !_removeFile(errorString)) {
            return false;
        }
        _saving = true;
        for (qsizetype i = 0; i < _repository->sources().count(); i++) {
            if (_repository->sources()[i]->key() == path) {
                _repository->removeSource(static_cast<int>(i));
                break;
            }
        }
        _saving = false;
        if (!_zones.isEmpty()) {
            _zones.clear();
            emit zonesChanged();
        }
        return true;
    }

    // Read back first, which normalizes the zones like any source and rejects unusable shapes
    const QByteArray document = GeoZoneEd318Writer::document(zones, tr(title), tr("Operator"), GeoZoneEnvironment::currentDateTimeUtc());
    GeoZoneParser::Result result;
    if (!GeoZoneParser::parse(document, result, errorString) || (result.zones.count() != zones.count())) {
        errorString = tr("The zone can't be saved: %1").arg(errorString.isEmpty() ? tr("its shape is not valid.") : errorString);
        return false;
    }

    // Never over a file which couldn't be read, whose zones would be lost
    if (!_loadError.isEmpty()) {
        if (!_setUnreadableFileAside(errorString)) {
            return false;
        }
        _setLoadError(QString());
    }
    QSaveFile file(path);
    if (!QDir().mkpath(QFileInfo(path).absolutePath()) || !file.open(QIODevice::WriteOnly) || (file.write(document) != document.size()) || !file.commit()) {
        errorString = tr("The zones can't be saved to %1: %2").arg(path, file.errorString());
        return false;
    }
    _saving = true;
    const bool loaded = _repository->loadFile(path, errorString);
    _saving = false;
    if (!loaded) {
        return false;
    }
    _zones = result.zones;
    emit zonesChanged();
    return true;
}

QVariantMap GeoZoneOperatorZones::properties(const GeoZone::Data &zone)
{
    const GeoZone::Volume volume = zone.volumes.value(0);
    const auto reference = [](GeoZone::VerticalReference value) {
        return (value == GeoZone::VerticalReference::MeanSeaLevel) ? QStringLiteral("AMSL") : QStringLiteral("AGL");
    };
    return QVariantMap{
        { QStringLiteral("name"), zone.name },
        { QStringLiteral("restriction"), static_cast<int>(zone.restriction) },
        { QStringLiteral("message"), zone.message },
        { QStringLiteral("lower"), _meters(volume.lowerLimit, volume.feet) },
        { QStringLiteral("upper"), _meters(volume.upperLimit, volume.feet) },
        { QStringLiteral("lowerReference"), reference(volume.lowerReference) },
        { QStringLiteral("upperReference"), reference(volume.upperReference) },
    };
}

GeoZone::Data GeoZoneOperatorZones::zoneFromProperties(const QVariantMap &properties, const GeoZone::Volume &shape, const QString &identifier)
{
    GeoZone::Data zone;
    zone.identifier = identifier;
    zone.name = properties.value(QStringLiteral("name")).toString().trimmed();
    if (zone.name.isEmpty()) {
        zone.name = tr("My zone");
    }
    bool ok = false;
    const int restriction = properties.value(QStringLiteral("restriction")).toInt(&ok);
    zone.restriction = (ok && (restriction >= GeoZone::Prohibited) && (restriction <= GeoZone::NoRestriction)) ? static_cast<GeoZone::Restriction>(restriction) : GeoZone::Prohibited;
    zone.restrictionCode = GeoZoneEd318Writer::restrictionCode(zone.restriction);
    zone.reasons.append(QStringLiteral("OTHER"));
    zone.otherReasonInfo = tr("Drawn by the operator");
    zone.message = properties.value(QStringLiteral("message")).toString().trimmed();
    zone.authorities.append(tr("Operator"));

    GeoZone::Volume volume;
    volume.isCircle = shape.isCircle;
    volume.center = shape.center;
    volume.radius = shape.radius;
    volume.rings = shape.rings;
    volume.lowerLimit = _number(properties.value(QStringLiteral("lower")));
    volume.upperLimit = _number(properties.value(QStringLiteral("upper")));
    // Limits are written with their references, a zone without limits applies at all altitudes
    if (!std::isnan(volume.lowerLimit) || !std::isnan(volume.upperLimit)) {
        const auto code = [&properties](const char *key) {
            return (properties.value(QLatin1String(key)).toString() == QStringLiteral("AMSL")) ? QStringLiteral("AMSL") : QStringLiteral("AGL");
        };
        volume.setReferences(code("lowerReference"), code("upperReference"));
    }
    zone.volumes.append(volume);
    return zone;
}

void GeoZoneOperatorZones::_sourceChanged(GeoZoneSource *source)
{
    // Only the source of the zones, once it is removed: the removal of another source, like before the source of the
    // zones is loaded at the start, must leave the file
    if (_saving || !source || (source->key() != fileName()) || _repository->sources().contains(source)) {
        return;
    }
    // A file which couldn't be read is kept, see load
    if (!_loadError.isEmpty()) {
        return;
    }
    // The user removed the source, with the zones
    cancelEdit();
    QString errorString;
    if (!_removeFile(errorString)) {
        // The zones left in the file stay loaded, rather than come back at the next start without their source
        emit failed(errorString);
        QMetaObject::invokeMethod(this, &GeoZoneOperatorZones::_reloadSource, Qt::QueuedConnection);
        return;
    }
    if (!_zones.isEmpty()) {
        _zones.clear();
        emit zonesChanged();
    }
}

void GeoZoneOperatorZones::_reloadSource()
{
    if (_zones.isEmpty() || _repository->hasSource(fileName())) {
        return;
    }
    QString errorString;
    _saving = true;
    const bool loaded = _repository->loadFile(fileName(), errorString);
    _saving = false;
    if (!loaded) {
        emit failed(errorString);
    }
}
