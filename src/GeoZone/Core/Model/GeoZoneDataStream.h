/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QDataStream>
#include <QtCore/QIODevice>
#include <QtCore/QList>
#include <QtCore/QPointF>
#include <QtCore/QStringList>
#include <QtCore/QtEndian>

#include <limits>
#include <type_traits>

/// Helpers of the binary serialization of geozones, see GeoZoneParseCache. Readers check the counts they read against
/// the bytes left in the stream before allocating, so damaged data fails the stream instead of exhausting the memory.
/// Values out of range set the stream status to ReadCorruptData.
namespace GeoZoneDataStream {

inline void writeCount(QDataStream &stream, qsizetype count)
{
    stream << static_cast<quint32>(count);
}

/// Reads a count of elements taking at least minBytes each
/// @return false, with the stream failed, when the bytes left can't hold them
inline bool readCount(QDataStream &stream, qint64 minBytes, qsizetype &count)
{
    quint32 value = 0;
    stream >> value;
    if (stream.status() != QDataStream::Ok) {
        return false;
    }
    const QIODevice *const device = stream.device();
    if (!device || ((static_cast<qint64>(value) * minBytes) > device->bytesAvailable())) {
        stream.setStatus(QDataStream::ReadCorruptData);
        return false;
    }
    count = static_cast<qsizetype>(value);
    return true;
}

/// Reads an enumeration written as an integer
/// @return false, with the stream failed, when the value isn't between 0 and last
template<typename Enum>
bool readEnum(QDataStream &stream, Enum &value, Enum last)
{
    quint8 number = 0;
    stream >> number;
    if ((stream.status() != QDataStream::Ok) || (number > static_cast<quint8>(last))) {
        stream.setStatus(QDataStream::ReadCorruptData);
        return false;
    }
    value = static_cast<Enum>(number);
    return true;
}

template<typename T>
void writeList(QDataStream &stream, const QList<T> &list)
{
    writeCount(stream, list.count());
    for (const T &value : list) {
        stream << value;
    }
}

/// Reads a list written by writeList. The list grows as its elements are read rather than for the count read.
/// @param minBytes Fewest bytes an element takes in the stream
template<typename T>
bool readList(QDataStream &stream, QList<T> &list, qint64 minBytes)
{
    list.clear();
    qsizetype count = 0;
    if (!readCount(stream, minBytes, count)) {
        return false;
    }
    for (qsizetype i = 0; i < count; i++) {
        T value;
        stream >> value;
        if (stream.status() != QDataStream::Ok) {
            list.clear();
            return false;
        }
        list.append(std::move(value));
    }
    return true;
}

/// Strings take at least their length
inline bool readStrings(QDataStream &stream, QStringList &list)
{
    return readList(stream, list, sizeof(quint32));
}

/// Writes points as one block of little endian doubles, much faster than one value at a time
inline void writePoints(QDataStream &stream, const QList<QPointF> &points)
{
    writeCount(stream, points.count());
    if constexpr ((Q_BYTE_ORDER == Q_LITTLE_ENDIAN) && std::is_same_v<qreal, double> && (sizeof(QPointF) == (2 * sizeof(double)))) {
        (void) stream.writeRawData(reinterpret_cast<const char*>(points.constData()), static_cast<int>(points.count() * sizeof(QPointF)));
    } else {
        QByteArray bytes(points.count() * 2 * sizeof(double), Qt::Uninitialized);
        char *data = bytes.data();
        for (const QPointF &point : points) {
            qToLittleEndian<double>(point.x(), data);
            qToLittleEndian<double>(point.y(), data + sizeof(double));
            data += 2 * sizeof(double);
        }
        (void) stream.writeRawData(bytes.constData(), static_cast<int>(bytes.size()));
    }
}

inline bool readPoints(QDataStream &stream, QList<QPointF> &points)
{
    points.clear();
    qsizetype count = 0;
    if (!readCount(stream, 2 * sizeof(double), count)) {
        return false;
    }
    const qint64 size = count * 2 * sizeof(double);
    if (size > std::numeric_limits<int>::max()) {
        stream.setStatus(QDataStream::ReadCorruptData);
        return false;
    }
    if constexpr ((Q_BYTE_ORDER == Q_LITTLE_ENDIAN) && std::is_same_v<qreal, double> && (sizeof(QPointF) == (2 * sizeof(double)))) {
        points.resize(count);
        if (stream.readRawData(reinterpret_cast<char*>(points.data()), static_cast<int>(size)) != size) {
            points.clear();
            stream.setStatus(QDataStream::ReadPastEnd);
            return false;
        }
    } else {
        QByteArray bytes(size, Qt::Uninitialized);
        if (stream.readRawData(bytes.data(), static_cast<int>(size)) != size) {
            stream.setStatus(QDataStream::ReadPastEnd);
            return false;
        }
        points.reserve(count);
        const char *data = bytes.constData();
        for (qsizetype i = 0; i < count; i++) {
            points.append(QPointF(qFromLittleEndian<double>(data), qFromLittleEndian<double>(data + sizeof(double))));
            data += 2 * sizeof(double);
        }
    }
    return true;
}

} // namespace GeoZoneDataStream
