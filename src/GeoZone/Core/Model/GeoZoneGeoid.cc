/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneGeoid.h"

#include <QtCore/QFile>
#include <QtCore/QList>
#include <QtCore/QtEndian>

#include <algorithm>
#include <cmath>

namespace GeoZoneGeometry {

double geoidHeight(const QGeoCoordinate &coordinate)
{
    constexpr int rows = 181;
    constexpr int columns = 360;
    // Decimeters, rows from 90N to 90S, columns from 0E eastwards
    static const QList<qint16> grid = []() {
        QList<qint16> values;
        QFile file(QStringLiteral(":/geoid/egm96_1deg.bin"));
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = file.readAll();
            if (bytes.size() == (rows * columns * 2)) {
                values.reserve(rows * columns);
                for (int i = 0; i < (rows * columns); i++) {
                    values.append(qFromLittleEndian<qint16>(bytes.constData() + (i * 2)));
                }
            }
        }
        return values;
    }();

    if (!coordinate.isValid() || grid.isEmpty()) {
        return qQNaN();
    }

    const double row = 90.0 - coordinate.latitude();
    const double column = std::fmod(coordinate.longitude() + 360.0, 360.0);
    const int row0 = std::clamp(static_cast<int>(std::floor(row)), 0, rows - 1);
    const int column0 = static_cast<int>(std::floor(column)) % columns;
    const int row1 = std::min(row0 + 1, rows - 1);
    const int column1 = (column0 + 1) % columns;
    const double rowFraction = std::clamp(row - row0, 0.0, 1.0);
    const double columnFraction = column - std::floor(column);
    const auto value = [](int r, int c) { return grid[(r * columns) + c] / 10.0; };

    return (value(row0, column0) * (1 - rowFraction) * (1 - columnFraction)) +
           (value(row0, column1) * (1 - rowFraction) * columnFraction) +
           (value(row1, column0) * rowFraction * (1 - columnFraction)) +
           (value(row1, column1) * rowFraction * columnFraction);
}

} // namespace GeoZoneGeometry
