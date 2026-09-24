/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/
#include "TaisyncInfo.h"
#include <QNetworkProxy>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QDateTime>
#include "SettingsManager.h"
#include "AppSettings.h"
#include "MultiVehicleManager.h"
#include "Vehicle.h"

TaisyncInfo::TaisyncInfo()
{
    QHostAddress host = QHostAddress::AnyIPv4;
    udpSocket = new QUdpSocket(this);
    udpSocket->setProxy(QNetworkProxy::NoProxy);
    _connectStatus = udpSocket->bind(host, 16789, QAbstractSocket::ReuseAddressHint | QUdpSocket::ShareAddress);
    if (_connectStatus)
    {
        udpSocket->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption,     64 * 1024);
        udpSocket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption,  128 * 1024);

        QObject::connect(udpSocket, &QUdpSocket::readyRead, this, &TaisyncInfo::readBytes);
        qDebug () << "udp create success\n";
    }
    _logName.clear();

    Fact *saveDataFact = SettingsManager::instance()->appSettings()->taisyncFlyDataSave();
    if (saveDataFact) {
        connect(saveDataFact, &Fact::valueChanged, this, &TaisyncInfo::onAutoSaveChanged);
        onAutoSaveChanged();
    }

    (void) connect(MultiVehicleManager::instance(), &MultiVehicleManager::activeVehicleChanged, this, &TaisyncInfo::_setActiveVehicle);
    _setActiveVehicle(MultiVehicleManager::instance()->activeVehicle());
}

TaisyncInfo::~TaisyncInfo(void)
{
    if (udpSocket)
    {
        udpSocket->close();
    }
}

void TaisyncInfo::readBytes()
{
    if (!udpSocket)
    {
        return;
    }

    QByteArray databuffer;

    while(udpSocket->hasPendingDatagrams())
    {
        QByteArray datagram;

        datagram.resize(udpSocket->pendingDatagramSize());

        QHostAddress    sender;
        quint16         senderPort;

        qint64 slen = udpSocket->readDatagram(datagram.data(),datagram.size(), &sender, &senderPort);
        if (slen == -1)
        {
            break;
        }

        databuffer.append(datagram);
        if (databuffer.size() > 10 * 1024)
        {
            databuffer.clear();
        }
        else
        {
            receiveParse(databuffer);
        }
    }
}

void TaisyncInfo::deleteOldLogs()
{
    // only save N log file max
    // if txt file num > N, delete all the history files
    static int maxFileNumber = 30;
    QDir dir(SettingsManager::instance()->appSettings()->linkLogSavePath());
    if (!dir.exists()) return;
    QStringList filters;
    filters << "*.txt";
    dir.setFilter(QDir::Files | QDir::Readable | QDir::NoSymLinks | QDir::Writable);
    dir.setNameFilters(filters);
    QStringList fileList = dir.entryList();
    if (fileList.size() >= maxFileNumber) {
        // sort by filename
        const auto getNum = [](const QString&s){
            bool ok;
            QString numString = s.section('.',0,0);
            qulonglong num = numString.toULongLong(&ok);
            return num;
        };
        std::sort(fileList.begin(), fileList.end(), [&getNum](const QString& a, const QString& b) {
            return getNum(a) < getNum(b);
        });
        while(fileList.size() >= maxFileNumber) {
            QString fileName = fileList.first();
            fileList.pop_front();
            QString filePath = dir.absolutePath();
            filePath.append("/").append(fileName);
            qDebug() << "auto delete logfile:" << filePath;
            QFile::remove(filePath);
        }
    }
}

void TaisyncInfo::updateRssi(int rssi1, int rssi2)
{
    if (!_vehicle) return;

    int rssi = UINT8_MAX;
    // find available rssi value from 1&2
    if (rssi1 != UINT8_MAX) rssi = rssi1;
    if (rssi2 != UINT8_MAX) {
        if (rssi == UINT8_MAX) rssi = rssi2; // rssi1 is invalide, juse use rssi2
        else rssi = qMax(rssi, rssi2); // use more good rssi value
    }
    if (rssi != UINT8_MAX) {
        // rssi is negative, but it may received as a positive number
        if (rssi > 0) rssi = -rssi;

        // update rssi value to vehicle
        // convert rssi to 0%-100%
        const int min = -100; // 0%
        const int max = -50; // 100%
        rssi = qMax(rssi, min);
        rssi = qMin(rssi, max);
        rssi = (int)((float)(rssi - min) / (float)(max - min) * 100);
    }

    emit _vehicle->remoteControlRSSIChanged(rssi);
}

void TaisyncInfo::receiveParse(QByteArray b)
{
    QJsonParseError errJson;
    QJsonDocument jsonData = QJsonDocument::fromJson(b, &errJson);

    if (errJson.error != QJsonParseError::NoError)
    {
        return;
    }
    else
    {
        QJsonObject _dataObj = jsonData.object();
        // if (SettingsManager::instance()->appSettings()->taisyncFlyViewShow()->rawValue().toBool()) {
            const auto toString = [](const QJsonValue& value) -> QString {
                if (value.isString()) return value.toString().trimmed();
                if (value.isDouble()) return QString::number(value.toDouble());
                if (value.isBool()) return QStringLiteral("%1").arg(value.toBool() ? "true" : "false");
                if (value.isArray()) return QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact);
                if (value.isObject()) return QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
                if (value.isNull()) return "NULL";
                if (value.isUndefined()) return "UNDEFINED";
                return "UNKNOWN";
            };

            QJsonObject slaveObj = _dataObj;

            if (slaveObj.contains("pass_a"))
            {
                _airLDPCPass = toString(slaveObj.value("pass_a"));
                emit airLDPCPassChanged();
            }

            if (slaveObj.contains("failed_a"))
            {
                _airLDPCFailed = toString(slaveObj.value("failed_a"));
                emit airLDPCFailedChanged();
            }

            if (slaveObj.contains("snr_a"))
            {
                _airSNR = toString(slaveObj.value("snr_a"));
                emit airSNRChanged();
            }

            if (slaveObj.contains("rssi1_a"))
            {
                _airRSSI0 = toString(slaveObj.value("rssi1_a"));
                emit airRSSI0Changed();
            }

            if (slaveObj.contains("rssi2_a"))
            {
                _airRSSI1 = toString(slaveObj.value("rssi2_a"));
                emit airRSSI1Changed();
            }

            if (slaveObj.contains("pass_g"))
            {
                _gndLDPCPass = toString(slaveObj.value("pass_g"));
                emit gndLDPCPassChanged();
            }

            if (slaveObj.contains("failed_g"))
            {
                _gndLDPCFailed = toString(slaveObj.value("failed_g"));
                emit gndLDPCFailedChanged();
            }

            auto linkConnected = false;
            if (slaveObj.contains("snr_g"))
            {
                _gndSNR = toString(slaveObj.value("snr_g"));
                emit gndSNRChanged();
                linkConnected = _gndSNR.toInt() > 5;
            }

            int rssi1 = UINT8_MAX; // UINT8_MAX is invalid value
            int rssi2 = UINT8_MAX;
            if (slaveObj.contains("rssi1_g"))
            {
                _gndRSSI0 = toString(slaveObj.value("rssi1_g"));
                emit gndRSSI0Changed();
                if (linkConnected) {
                    bool ok;
                    auto value = _gndRSSI0.toInt(&ok);
                    if (ok) rssi1 = value;
                }
            }

            if (slaveObj.contains("rssi2_g"))
            {
                _gndRSSI1 = toString(slaveObj.value("rssi2_g"));
                emit gndRSSI1Changed();
                if (linkConnected) {
                    bool ok;
                    auto value = _gndRSSI1.toInt(&ok);
                    if (ok) rssi2 = value;
                }
            }
            updateRssi(rssi1, rssi2);

            if (slaveObj.contains("distance"))
            {
                _range = toString(slaveObj.value("distance"));
                emit rangeChanged();
            }

            if (_dataObj.contains("ethTx"))
            {
                _dataRate = toString(_dataObj.value("ethTx"));
                emit dataRateChanged();
            }

            // if (_dataObj.contains("lockCnt"))
            // {
            //     _lockCnt = toString(_dataObj.value("lockCnt"));
            //     emit lockCntChanged();
            // }

            if (_dataObj.contains("freq_rx"))
            {
                _currFreq = toString(_dataObj.value("freq_rx"));
                emit currFreqChanged();
            }

            if (slaveObj.contains("ant_a"))
            {
                _ant = toString(slaveObj.value("ant_a"));
                emit antChanged();
            }

            if (_dataObj.contains("ant_g"))
            {
                _antGnd = toString(_dataObj.value("ant_g"));
                emit antGndChanged();
            }

            if (_dataObj.contains("mcs"))
            {
                _mcs = toString(_dataObj.value("mcs"));
                emit mcsChanged();
            }

            // Parse data only showExtra
            if (SettingsManager::instance()->appSettings()->showExtra()) {
                QStringList noiseTitles, noiseValuesA, noiseValuesG;
                if (slaveObj.contains("noiseFloor_a")) {
                    QString s = toString(slaveObj.value("noiseFloor_a"));
                    if (!s.isEmpty()) {
                        noiseValuesA = s.split(",");
                        noiseValuesA.removeAll("");
                    }
                }
                if (_dataObj.contains("noiseFloor_g")) {
                    QString s = toString(_dataObj.value("noiseFloor_g"));
                    if (!s.isEmpty()) {
                        noiseValuesG = s.split(",");
                        noiseValuesG.removeAll("");
                    }
                }
                if (_dataObj.contains("freq_list")) {
                    QString s = toString(_dataObj.value("freq_list"));
                    if (!s.isEmpty()) {
                        noiseTitles = s.split(",");
                        noiseTitles.removeAll("");
                    }
                }
                _noiseTitles.clear();
                _noiseValuesA.clear();
                _noiseValuesG.clear();
#if (QT_VERSION >= QT_VERSION_CHECK(6, 6, 0))
                for (const auto& v : std::as_const(noiseTitles)) {
#else
                for (const auto& v : qAsConst(noiseTitles)) {
#endif
                    _noiseTitles << v;
                }
#if (QT_VERSION >= QT_VERSION_CHECK(6, 6, 0))
                for (const auto& v : std::as_const(noiseValuesA)) {
#else
                for (const auto& v : qAsConst(noiseValuesA)) {
#endif
                    _noiseValuesA << v;
                }
#if (QT_VERSION >= QT_VERSION_CHECK(6, 6, 0))
                for (const auto& v : std::as_const(noiseValuesG)) {
#else
                for (const auto& v : qAsConst(noiseValuesG)) {
#endif
                    _noiseValuesG << v;
                }
                emit noiseChanged();
            }
        // }

        if (_dataObj.contains("role"))
        {
            QString role = _dataObj.value("role").toString();
            SettingsManager::instance()->appSettings()->setIsPilot(role != "1");
        } else {
            SettingsManager::instance()->appSettings()->setIsPilot(true);
        }

        savelogInfo(b);
    }
}

void TaisyncInfo::startLogSave()
{
    // make sure log files under num limit
    deleteOldLogs();

    QString filePath,currDateTimeStr,currTimeStr;

    filePath = SettingsManager::instance()->appSettings()->linkLogSavePath();
    QDateTime currTime = QDateTime::currentDateTime();
    currDateTimeStr = currTime.toString("yyyyMMdd");
    currTimeStr = currTime.toString("hhmmss");
    QDir dir(filePath);

    if (!dir.exists())
    {
        bool folder = dir.mkdir(filePath);
        if (!folder)
        {
            qDebug()<<"log folder create failed:" << filePath;
            return;
        }
    }

    _logName = filePath.append("/").append(currDateTimeStr).append(currTimeStr).append(".txt");

    QFileInfo _info(_logName);
    if (_info.isFile())
    {
        qDebug()<<"file is exist";
    }
    else
    {
        QFile file(_logName);
        (void) file.open(QIODevice::ReadWrite|QIODevice::Append);

        if (file.isOpen())
        {
            QTextStream txOutput(&file);
            txOutput <<"time gndRssi0 gndRssi1 gndSnr gndLDPCPass gndLDPCFailed airRssi0 airRssi1 airSnr airLDPCPass airLDPCFailed rang rate lockCnt freq ant antGnd mcs"<<Qt::endl;
            file.close();
        }
    }
}

void TaisyncInfo::stopLogSave()
{
    QFile file(_logName);
    if (file.isOpen())
    {
        file.close();
        qDebug()<<"close file :"<<_logName;
    }
    else
    {
        qDebug()<<"log name"<<_logName;
    }

    _logName.clear();
}

void TaisyncInfo::savelogInfo(const QByteArray& log)
{
    if (!SettingsManager::instance()->appSettings()->taisyncFlyDataSave()->rawValue().toBool()) return;

    if (!_logName.isNull())
    {
        QFile file(_logName);
        (void) file.open(QIODevice::ReadWrite|QIODevice::Append);

        QDateTime   currDate = QDateTime::currentDateTime();
        QString currTimeStr = currDate.toString("hh:mm:ss");

        if (file.isOpen())
        {
            QTextStream txOutput(&file);
            txOutput <<currTimeStr.append(" ").append(log)<<Qt::endl;
            file.close();
            currTimeStr.clear();
        }
    }
}

void TaisyncInfo::onAutoSaveChanged()
{
    Fact *saveDataFact = SettingsManager::instance()->appSettings()->taisyncFlyDataSave();
    if (saveDataFact) {
        if (saveDataFact->rawValue().toBool()) {
            startLogSave();
        } else {
            stopLogSave();
        }
    }
}

void TaisyncInfo::_setActiveVehicle(Vehicle *vehicle)
{
    _vehicle = vehicle;
}

