/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#pragma once

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QFutureWatcher>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>

#include <functional>
#include <type_traits>
#include <utility>

#include "GeoZoneCancelToken.h"

class MissionController;

/// Follows a planned mission and evaluates it on the thread pool, applying the results on the GUI thread, for the
/// mission check, the altitude profile and the route planner of the Plan View.
/// Changes of the mission, and those the owner reports with schedule(), settle for a delay before triggered() asks the
/// owner to start an evaluation with run() or watch(). Each evaluation belongs to a generation: starting another one
/// or invalidate(), like when the zones or the mission controller change, outdates those running, whose results are
/// then dropped instead of applied.
class GeoZoneMissionWorker : public QObject
{
    Q_OBJECT

public:
    /// @param delayMSecs How long changes settle before an evaluation is triggered
    explicit GeoZoneMissionWorker(int delayMSecs, QObject *parent = nullptr);
    ~GeoZoneMissionWorker() override;

    MissionController *missionController() const;
    /// Follows the changes of the controller's mission, which schedule an evaluation, and outdates the evaluations of
    /// the previous one
    /// @return false if the controller didn't change
    bool setMissionController(MissionController *missionController);

    void setDelay(int msecs) { _timer.setInterval(msecs); }
    /// While inactive, nothing is scheduled, like while the results aren't shown
    bool active() const { return _active; }
    /// Stops the scheduled evaluation when deactivated
    void setActive(bool active);

    /// Triggers an evaluation after the delay, restarting it, while active
    void schedule();
    /// Triggers the scheduled evaluation now
    void trigger();
    /// @return true while an evaluation is scheduled
    bool scheduled() const { return _timer.isActive(); }
    /// Outdates the running evaluations, whose results are dropped, and cancels their token
    void invalidate();
    /// Stops the scheduled evaluation and outdates the running ones, like before another evaluation starts
    void stop();
    /// @return true if an evaluation of the current generation was applied and none is scheduled or running, so that
    ///         the owner's result is that of its current inputs
    bool settled() const { return !_timer.isActive() && (_running == 0) && (_applied == _generation); }
    /// @return true while evaluations of the current generation are running
    bool busy() const { return _running > 0; }
    /// Cancelled when the evaluations of the current generation are outdated
    const GeoZoneCancelToken &cancelToken() const { return _cancel; }

    /// Starts a new generation, runs compute on the thread pool, then apply with its result on this thread unless
    /// outdated meanwhile. Compute must only use what it captured by value.
    template <typename Compute, typename Apply>
    void run(Compute compute, Apply apply)
    {
        stop();
        watch(QtConcurrent::run(std::move(compute)), std::move(apply));
    }

    /// Starts a new generation and evaluates on this thread, applying the result immediately
    template <typename Compute, typename Apply>
    void runNow(Compute compute, Apply apply)
    {
        stop();
        apply(compute());
        _applied = _generation;
    }

    /// Applies the result of a computation of the current generation, like another round of it, when it finishes
    /// unless outdated meanwhile. Futures without a result, like cancelled ones, aren't applied.
    /// @param progress Called with the progress values of the future while current
    template <typename Result, typename Apply>
    void watch(const QFuture<Result> &future, Apply apply, const std::function<void(int)> &progress = nullptr)
    {
        auto *const watcher = new QFutureWatcher<Result>(this);
        const quint64 generation = _generation;
        _running++;
        if (progress) {
            (void) connect(watcher, &QFutureWatcherBase::progressValueChanged, this, [this, generation, progress](int value) {
                if (generation == _generation) {
                    progress(value);
                }
            });
        }
        (void) connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, apply = std::move(apply)]() {
            watcher->deleteLater();
            if (generation != _generation) {
                return;
            }
            _running--;
            if (watcher->future().resultCount() > 0) {
                apply(watcher->result());
            }
            if (_running == 0) {
                _applied = generation;
            }
        });
        watcher->setFuture(future);
    }

signals:
    /// The scheduled evaluation is due, the owner starts it
    void triggered();

private:
    QPointer<MissionController> _missionController;
    QTimer _timer;
    bool _active = true;
    quint64 _generation = 1;    ///< Of the evaluations whose results are wanted
    quint64 _applied = 0;       ///< Generation of the last result applied, none yet
    int _running = 0;           ///< Evaluations of the current generation
    GeoZoneCancelToken _cancel;
};
