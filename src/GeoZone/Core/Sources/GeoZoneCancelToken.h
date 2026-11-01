/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <atomic>
#include <memory>

/// Tells the stages of a fetch or load, on any thread, that their result is no longer wanted: the source was
/// removed or loaded again, or the application is closing. Copies share the flag.
class GeoZoneCancelToken
{
public:
    GeoZoneCancelToken() = default;

    void cancel() const { _cancelled->store(true); }
    bool isCancelled() const { return _cancelled->load(); }

    /// @return true if both are copies of the same token
    bool operator==(const GeoZoneCancelToken &other) const { return _cancelled == other._cancelled; }

private:
    std::shared_ptr<std::atomic_bool> _cancelled = std::make_shared<std::atomic_bool>(false);
};
