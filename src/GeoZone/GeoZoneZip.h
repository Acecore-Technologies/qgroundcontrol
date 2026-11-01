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
#include <QtCore/QString>
#include <QtCore/QStringList>

/// Reads one file of a zip archive, like the dataset of a feed, without extracting the others. Sizes are limited
/// while inflating, whatever the archive declares, so a small archive can't expand to fill the memory.
namespace GeoZoneZip {

constexpr int maxEntries = 1000;

/// @return The largest file of the archive with one of the suffixes, inflated, empty on errors
/// @param maxBytes Larger files, or archives declaring them, are rejected
QByteArray largestFile(const QByteArray &archive, const QStringList &suffixes, qint64 maxBytes, QString &errorString);

} // namespace GeoZoneZip
