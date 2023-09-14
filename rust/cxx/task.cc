#include <atomic>
#include <cassert>
#include <seastar/core/reactor.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/task.hh>

#include <seastar/util/log.hh>

static seastar::logger tlogger("rust_task_logger");

class rust_task;
using rust_future_poll_fn = int (*)(rust_task* task, void* fut);

enum class scheduling_state : unsigned {
    // Neither scheduled nor running
    idle = 0,

    // Scheduled for execution - but not running yet
    scheduled = 1,
    
    // Being executed right now
    executing = 2,

    // Being executed, but need to be rescheduled after the poll finishes
    executing_with_pending_schedule = 3,

    // The future returned a value and the task is finished.
    // If the task still exists at this point, it is because it is held
    // alive by the wakers.
    done = 4,
};

std::ostream& operator<<(std::ostream& os, scheduling_state ss) {
    switch (ss) {
    case scheduling_state::idle:
        return os << "scheduling_state::idle";
    case scheduling_state::scheduled:
        return os << "scheduling_state::scheduled";
    case scheduling_state::executing:
        return os << "scheduling_state::executing";
    case scheduling_state::executing_with_pending_schedule:
        return os << "scheduling_state::executing_with_pending_schedule";
    case scheduling_state::done:
        return os << "scheduling_state::done";
    }
}

// Polls the rust task to completion.
// All methods are not thread-safe and must be called from the thread
// the task was originally created on, unless marked as thread-safe.
class rust_task final : public seastar::task {
private:
    // Reference count, used by wakers referring to this task to keep it alive.
    //
    // Unfortunately, it must be an atomic because the Waker interface allows
    // wakers to be sent to other threads (shards) and used/cloned there.
    // Such cross-shard use may happen if someone uses third-party crates for
    // communication between shards, though we highly discourage this pattern.
    // Fortunately, we can heavily optimize towards the most common use case
    // where cross-shard use doesn't happen by using relaxed atomic operations.
    // TODO: Why further synchronization isn't necessary?
    //
    // The counter starts as `1` and is decremented when the task finishes.
    std::atomic<uint64_t> _ref_count{1};

    // It should be possible for a Rust waker to be woken from any thread,
    // no matter whether managed by the runtime (shards) or others.
    // In order to satisfy this promise, we preserve the shard number.
    const unsigned _origin_shard;

    // The state of the task with respect to scheduling.
    // The task must be aware of this in order to properly handle wake-ups.
    scheduling_state _sched_state = scheduling_state::idle;

    // A Rust function used to poll the future object.
    // Deallocates the _rust_future after the last poll.
    const rust_future_poll_fn _poll_fn;

    // Pointer to the Rust future.
    void* _rust_future;

private:
    void do_wake() noexcept {
        assert(_origin_shard == seastar::this_shard_id());

        tlogger.trace("Waking task {} at state {}", fmt::ptr(this), _sched_state);

        switch (_sched_state) {
        case scheduling_state::idle:
            // Schedule the task for execution
            tlogger.trace("Task {} is being scheduled", fmt::ptr(this));
            seastar::schedule(this);
            _sched_state = scheduling_state::scheduled;
            break;

        case scheduling_state::scheduled:
            // No need to do anything, the task is already scheduled.
            break;

        case scheduling_state::executing:
            // The task is being executed right now, but we must remember
            // to re-schedule it after it finishes executing.
            _sched_state = scheduling_state::executing_with_pending_schedule;
            break;
        
        case scheduling_state::executing_with_pending_schedule:
            // No need to do anything, the task will be scheduled after
            // it finishes being polled.
            break;

        case scheduling_state::done:
            // No need to do anything. The task was finished and waking it
            // won't have any effect.
            break;
        }
    }

    // Thread-safe
    inline bool is_on_right_thread() const {
        return seastar::this_shard_id() == _origin_shard;
    }

    // Thread-safe
    template<typename F>
    void call_on_origin_thread(F&& f) noexcept {
        // TODO: Increment some metrics?
        (void)seastar::smp::submit_to(_origin_shard, std::move(f));
    }

    inline void dec_ref_local() noexcept {
        auto count = _ref_count.fetch_sub(1, std::memory_order_relaxed);
        tlogger.trace("Decremented reference count for {} to {}", fmt::ptr(this), count - 1);
        if (count == 1) {
            delete this;
        }
    }

public:
    rust_task(rust_future_poll_fn poll_fn, void* fut)
            : _origin_shard(seastar::this_shard_id())
            , _poll_fn(poll_fn)
            , _rust_future(fut) {
    }

    virtual void run_and_dispose() noexcept override final {
        tlogger.trace("Running task {}", fmt::ptr(this));

        assert(is_on_right_thread());
        assert(_sched_state != scheduling_state::done);

        _sched_state = scheduling_state::executing;

        const bool finished = _poll_fn(this, _rust_future) != 0;

        if (finished) {
            tlogger.trace("Task {} is finished", fmt::ptr(this));
            _sched_state = scheduling_state::done;
            dec_ref_local();
            return;
        }

        if (_sched_state == scheduling_state::executing_with_pending_schedule) {
            // The task was woken while it was being executed.
            // Schedule the task here again.
            tlogger.trace("Task {} is being scheduled again", fmt::ptr(this));
            seastar::schedule(this);
            _sched_state = scheduling_state::scheduled;
        } else {
            tlogger.trace("Task {} was preempted, currently waits", fmt::ptr(this));
            _sched_state = scheduling_state::idle;
        }
    }

    virtual seastar::task* waiting_task() noexcept override final {
        // This information is unavailable, unfortunately
        // With a more complicated implementation of rust_task that holds a promise
        // this could be possible. Alternatively, we might teach the rust_task to peek
        // into the rust future and retrieve the promise, and then the task.
        return nullptr;
    }

    // Thread-safe
    inline void inc_ref() noexcept {
        auto count = _ref_count.fetch_add(1, std::memory_order_relaxed);
        tlogger.trace("Incremented reference count for {} to {}", fmt::ptr(this), count + 1);
    }

    // Thread-safe
    inline void dec_ref() noexcept {
        auto count = _ref_count.fetch_sub(1, std::memory_order_relaxed);
        tlogger.trace("Decremented reference count for {} to {}", fmt::ptr(this), count - 1);
        if (count == 1) {
            call_on_origin_thread([this] () noexcept {
                delete this;
            });
        }
    }

    // Wakes this object and decreases the reference count.
    // Thread-safe
    void wake() noexcept {
        // This operation consumes the waker, so the lambda in submit_to
        // can assume ownership and release after it executes.
        call_on_origin_thread([this] () noexcept {
            do_wake();
            dec_ref_local();
        });
    }

    // Wakes this object, ultimately keeping the reference count the same.
    // Thread-safe
    void wake_by_ref() noexcept {
        if (is_on_right_thread()) {
            // We can get away without changing the reference count
            do_wake();
        } else {
            // We must increase reference count here so that nobody
            // decreases it in the meantime when the waking task
            // waits to be scheduled on another shard.
            inc_ref();
            wake(); // <- this will decrease the reference count
        }
    }
};

extern "C" {

void seastar_rs_task_spawn(rust_future_poll_fn poll_fn, void* rust_future) {
    (new rust_task(poll_fn, rust_future))->run_and_dispose();
}

void seastar_rs_waker_clone(void* data) {
    auto* task_base = reinterpret_cast<rust_task*>(data);
    task_base->inc_ref();
}

void seastar_rs_waker_wake(void* data) {
    auto* task_base = reinterpret_cast<rust_task*>(data);
    task_base->wake();
}

void seastar_rs_waker_wake_by_ref(void* data) {
    auto* task_base = reinterpret_cast<rust_task*>(data);
    task_base->wake_by_ref();
}

void seastar_rs_waker_drop(void* data) {
    auto* task_base = reinterpret_cast<rust_task*>(data);
    task_base->dec_ref();
}

}
