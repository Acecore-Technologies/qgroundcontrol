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
#include <QtCore/QCoreApplication>
#include <QtCore/QLoggingCategory>

#include <cstddef>
#include <cstdint>

/// Entry points of a libFuzzer target. Without libFuzzer, GeoZoneFuzzDriver.cc calls them for the files of a corpus.
extern "C" int LLVMFuzzerInitialize(int *argc, char ***argv);
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

namespace GeoZoneFuzz {

/// Creates the application the parsers translate their messages with and silences their logging, which would slow
/// the fuzzing down
inline void initialize(int *argc, char ***argv)
{
    static QCoreApplication app(*argc, *argv);
    QLoggingCategory::setFilterRules(QStringLiteral("*=false"));
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &) {});
}

/// @return The input without a copy, valid during the call of LLVMFuzzerTestOneInput
inline QByteArray bytes(const uint8_t *data, size_t size)
{
    return QByteArray::fromRawData(reinterpret_cast<const char*>(data), static_cast<qsizetype>(size));
}

} // namespace GeoZoneFuzz
