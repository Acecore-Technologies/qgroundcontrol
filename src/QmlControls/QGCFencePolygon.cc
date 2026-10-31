/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "QGCFencePolygon.h"
#include "JsonHelper.h"

QGCFencePolygon::QGCFencePolygon(bool inclusion, QObject* parent)
    : QGCMapPolygon (parent)
    , _inclusion    (inclusion)
{
    _init();
}

QGCFencePolygon::QGCFencePolygon(const QGCFencePolygon& other, QObject* parent)
    : QGCMapPolygon (other, parent)
    , _inclusion    (other._inclusion)
    , _name         (other._name)
    , _geoZone      (other._geoZone)
{
    _init();
}

void QGCFencePolygon::_init(void)
{
    connect(this, &QGCFencePolygon::inclusionChanged, this, &QGCFencePolygon::_setDirty);
    connect(this, &QGCFencePolygon::nameChanged,      this, &QGCFencePolygon::_setDirty);
    connect(this, &QGCFencePolygon::geoZoneChanged,   this, &QGCFencePolygon::_setDirty);
}

const QGCFencePolygon& QGCFencePolygon::operator=(const QGCFencePolygon& other)
{
    QGCMapPolygon::operator=(other);

    setInclusion(other._inclusion);
    setName(other._name);
    setGeoZone(other._geoZone);

    return *this;
}

void QGCFencePolygon::_setDirty(void)
{
    setDirty(true);
}

void QGCFencePolygon::saveToJson(QJsonObject& json)
{
    json[JsonHelper::jsonVersionKey] = _jsonCurrentVersion;
    json[_jsonInclusionKey] = _inclusion;
    if (!_name.isEmpty()) {
        json[_jsonNameKey] = _name;
    }
    if (_geoZone) {
        json[_jsonGeoZoneKey] = true;
    }
    QGCMapPolygon::saveToJson(json);
}

bool QGCFencePolygon::loadFromJson(const QJsonObject& json, bool required, QString& errorString)
{
    errorString.clear();

    QList<JsonHelper::KeyValidateInfo> keyInfoList = {
        { JsonHelper::jsonVersionKey,   QJsonValue::Double, true },
        { _jsonInclusionKey,            QJsonValue::Bool,   true },
    };
    if (!JsonHelper::validateKeys(json, keyInfoList, errorString)) {
        return false;
    }

    if (json[JsonHelper::jsonVersionKey].toInt() != _jsonCurrentVersion) {
        errorString = tr("GeoFence Polygon only supports version %1").arg(_jsonCurrentVersion);
        return false;
    }

    // Set first, since loading the polygon clears the dirty state
    setName(json[_jsonNameKey].toString());
    setGeoZone(json[_jsonGeoZoneKey].toBool());

    if (!QGCMapPolygon::loadFromJson(json, required, errorString)) {
        return false;
    }

    setInclusion(json[_jsonInclusionKey].toBool());

    return true;
}

void QGCFencePolygon::setInclusion(bool inclusion)
{
    if (inclusion != _inclusion) {
        _inclusion = inclusion;
        emit inclusionChanged(inclusion);
    }
}

void QGCFencePolygon::setName(const QString& name)
{
    if (name != _name) {
        _name = name;
        emit nameChanged(name);
    }
}

void QGCFencePolygon::setGeoZone(bool geoZone)
{
    if (geoZone != _geoZone) {
        _geoZone = geoZone;
        emit geoZoneChanged(geoZone);
    }
}
