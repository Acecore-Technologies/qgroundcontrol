/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFileInfo>
#include <QtCore/QStringList>

/// Input files of the command line tools of the geozone tests, the fuzz driver and geozone-validate
namespace GeoZoneInputFiles {

/// @return The files given and those of the directories given, recursively, sorted. Options, which start with a
///         dash, are skipped.
inline QStringList collect(const QStringList &arguments)
{
    QStringList files;
    for (const QString &path : arguments) {
        if (path.startsWith(QLatin1Char('-'))) {
            continue;
        }
        if (QFileInfo(path).isDir()) {
            QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                files.append(it.next());
            }
        } else {
            files.append(path);
        }
    }
    files.sort();
    return files;
}

} // namespace GeoZoneInputFiles
