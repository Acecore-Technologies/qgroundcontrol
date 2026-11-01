/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtPositioning/QGeoCoordinate>

#include <memory>

#include "GeoZoneCatalog.h"
#include "GeoZoneParser.h"
#include "GeoZoneSource.h"

/// Benchmarks of the geozone core with 10000 synthetic zones over central Europe, the size of a large national
/// dataset: parsing, loading, indexing, the queries of maps and alerts, and the evaluation of a long mission.
/// Run on their own, see test/GeoZone/README.md.
class GeoZoneBenchmark : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void _benchmarkParse();
    void _benchmarkLoadSource();
    void _benchmarkCatalogRebuild();
    void _benchmarkZonesAt();
    void _benchmarkZonesInArea();
    void _benchmarkPathEvaluation();

private:
    QByteArray _document;                       ///< ED-269 file with the zones
    GeoZoneParser::Result _result;
    std::unique_ptr<GeoZoneSource> _source;
    GeoZoneCatalog _catalog;
    QList<QGeoCoordinate> _points;              ///< Positions queried, inside and between zones
};
