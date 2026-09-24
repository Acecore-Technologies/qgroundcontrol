/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/
#ifndef __TAISYNCINFO_H__
#define __TAISYNCINFO_H__
#include <QObject>
#include <QUdpSocket>

class Vehicle;

class TaisyncInfo : public QObject
{
    Q_OBJECT

public:
    Q_PROPERTY(bool online          READ online             NOTIFY  onlineChanged)

    Q_PROPERTY(QString airRSSI0         READ airRSSI0           NOTIFY airRSSI0Changed)
    Q_PROPERTY(QString airRSSI1         READ airRSSI1           NOTIFY airRSSI1Changed)
    Q_PROPERTY(QString airSNR           READ airSNR             NOTIFY airSNRChanged)
    Q_PROPERTY(QString airLDPCPass      READ airLDPCPass        NOTIFY airLDPCPassChanged)
    Q_PROPERTY(QString airLDPCFailed    READ airLDPCFailed      NOTIFY airLDPCFailedChanged)
    Q_PROPERTY(QString gndRSSI0         READ gndRSSI0           NOTIFY gndRSSI0Changed)
    Q_PROPERTY(QString gndRSSI1         READ gndRSSI1           NOTIFY gndRSSI1Changed)
    Q_PROPERTY(QString gndSNR           READ gndSNR             NOTIFY gndSNRChanged)
    Q_PROPERTY(QString gndLDPCPass      READ gndLDPCPass        NOTIFY gndLDPCPassChanged)
    Q_PROPERTY(QString gndLDPCFailed    READ gndLDPCFailed      NOTIFY gndLDPCFailedChanged)
    Q_PROPERTY(QString range            READ range              NOTIFY rangeChanged)
    Q_PROPERTY(QString dataRate         READ dataRate           NOTIFY dataRateChanged)
    Q_PROPERTY(QString currFreq     READ currFreq           NOTIFY currFreqChanged)
    Q_PROPERTY(QString ant          READ ant                NOTIFY antChanged)
    Q_PROPERTY(QString antGnd       READ antGnd             NOTIFY antGndChanged)
    Q_PROPERTY(QString mcs          READ mcs                NOTIFY mcsChanged)
    Q_PROPERTY(QString lockCnt          READ lockCnt            NOTIFY lockCntChanged)

    Q_PROPERTY(QVariantList noiseTitles          READ noiseTitles            NOTIFY noiseChanged)
    Q_PROPERTY(QVariantList noiseValuesA          READ noiseValuesA            NOTIFY noiseChanged)
    Q_PROPERTY(QVariantList noiseValuesG          READ noiseValuesG            NOTIFY noiseChanged)

    TaisyncInfo();
    ~TaisyncInfo();

    void receiveParse(QByteArray b);

    bool online() const {return _online;}

    QString airRSSI0() const {return _airRSSI0;}
    QString airRSSI1() const {return _airRSSI1;}
    QString airSNR() const {return _airSNR;}
    QString airLDPCPass() const {return _airLDPCPass;}
    QString airLDPCFailed() const {return _airLDPCFailed;}

    QString gndRSSI0() const {return _gndRSSI0;}
    QString gndRSSI1() const {return _gndRSSI1;}
    QString gndSNR() const {return _gndSNR;}
    QString gndLDPCPass() const {return _gndLDPCPass;}
    QString gndLDPCFailed() const {return _gndLDPCFailed;}
    QString range() const {return _range;}
    QString dataRate() const {return _dataRate;}
    QString lockCnt() const {return _lockCnt;}
    QString currFreq() {return _currFreq;}
    QString ant() {return _ant;}
    QString antGnd() {return _antGnd;}
    QString mcs() {return _mcs;}

    QVariantList noiseTitles() {return _noiseTitles;}
    QVariantList noiseValuesA() {return _noiseValuesA;}
    QVariantList noiseValuesG() {return _noiseValuesG;}

    void savelogInfo(const QByteArray& log);

    Q_INVOKABLE void startLogSave();
    Q_INVOKABLE void stopLogSave();

public slots:
    void readBytes(void);

private slots:
    void onAutoSaveChanged(void);

signals:
    void onlineChanged();
    void airRSSI0Changed();
    void airRSSI1Changed();
    void airSNRChanged();
    void airLDPCPassChanged();
    void airLDPCFailedChanged();
    void gndRSSI0Changed();
    void gndRSSI1Changed();
    void gndSNRChanged();
    void gndLDPCPassChanged();
    void gndLDPCFailedChanged();
    void rangeChanged();
    void dataRateChanged();
    void lockCntChanged();
    void currFreqChanged();
    void antChanged();
    void antGndChanged();
    void mcsChanged();
    void noiseChanged();

private:

    // auto delete old logs's file
    void deleteOldLogs();
    void updateRssi(int rssi1, int rssi2);

    QUdpSocket  *udpSocket;
    bool        _connectStatus;
    bool        _online = 0;
    QString         _airRSSI0;
    QString         _airRSSI1;
    QString         _airSNR;
    QString         _airLDPCPass;
    QString         _airLDPCFailed;
    QString         _gndRSSI0;
    QString         _gndRSSI1;
    QString         _gndSNR;
    QString         _gndLDPCPass;
    QString         _gndLDPCFailed;
    QString         _range;
    QString         _dataRate;
    QString         _lockCnt;
    QString     _currFreq;
    QString     _ant;
    QString     _antGnd;
    QString     _mcs;
    QString     _logName;

    QVariantList _noiseTitles;
    QVariantList _noiseValuesA;
    QVariantList _noiseValuesG;

private slots:
    void _setActiveVehicle(Vehicle *vehicle);
private:
    Vehicle *_vehicle = nullptr;

};
#endif
