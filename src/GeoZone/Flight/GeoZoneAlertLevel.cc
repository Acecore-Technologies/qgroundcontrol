/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZoneAlertLevel.h"

#include <QtCore/QCoreApplication>

namespace GeoZoneAlertLevel {

QString text(Level level)
{
    switch (level) {
    case Warning:
        return QCoreApplication::translate("GeoZoneAlertLevel", "WARNING");
    case Caution:
        return QCoreApplication::translate("GeoZoneAlertLevel", "CAUTION");
    case Advisory:
        break;
    }
    return QCoreApplication::translate("GeoZoneAlertLevel", "ADVISORY");
}

QString code(Level level)
{
    switch (level) {
    case Warning:
        return QStringLiteral("warning");
    case Caution:
        return QStringLiteral("caution");
    case Advisory:
        break;
    }
    return QStringLiteral("advisory");
}

Level fromCode(const QString &code, bool *ok)
{
    for (const Level level : { Warning, Caution, Advisory }) {
        if (code == GeoZoneAlertLevel::code(level)) {
            if (ok) {
                *ok = true;
            }
            return level;
        }
    }
    if (ok) {
        *ok = false;
    }
    return Caution;
}

QString spokenPrefix(Level level)
{
    switch (level) {
    case Warning:
        return QCoreApplication::translate("GeoZoneAlertLevel", "Warning");
    case Caution:
        return QCoreApplication::translate("GeoZoneAlertLevel", "Caution");
    case Advisory:
        break;
    }
    return QString();
}

} // namespace GeoZoneAlertLevel
