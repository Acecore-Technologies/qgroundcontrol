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
#include <QtCore/QByteArrayView>
#include <QtCore/QCryptographicHash>

/// The hashes of geozone data: SHA-256 where hashes are published or exchanged (downloads, packs, the ids of secrets
/// and keys), and BLAKE2b-256, several times faster, of the stored copies, as their parse caches and provenance record
namespace GeoZoneHash {

inline QByteArray sha256(QByteArrayView data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

inline QByteArray contentHash(QByteArrayView data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Blake2b_256);
}

} // namespace GeoZoneHash
