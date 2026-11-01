/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "GeoZone.h"

#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickWindow>
#include <QtQuickTest/quicktest.h>

/// Registers the parts of the QGroundControl.GeoZone module which the tested components use, without the
/// application: the GeoZone enums and the GeoZoneStyle singleton, loaded from its source. The other components need
/// the singletons of QGroundControl, see test/GeoZone/README.md.
class GeoZoneQmlTestSetup : public QObject
{
    Q_OBJECT

public slots:
    void applicationAvailable()
    {
        // Rendered on the CPU, so that the tests also grab images with the offscreen platform
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        (void) qmlRegisterUncreatableMetaObject(GeoZone::staticMetaObject, "QGroundControl.GeoZone", 1, 0, "GeoZone", QStringLiteral("Enums only"));
        (void) qmlRegisterSingletonType(QUrl(QStringLiteral("qrc:/geozone/GeoZoneStyle.qml")), "QGroundControl.GeoZone", 1, 0, "GeoZoneStyle");
    }
};

QUICK_TEST_MAIN_WITH_SETUP(geozone_qml, GeoZoneQmlTestSetup)

#include "GeoZoneQmlTestMain.moc"
