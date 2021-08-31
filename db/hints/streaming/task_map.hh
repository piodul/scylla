/*
 * Copyright (C) 2021-present ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <cassert>
#include <concepts>
#include <exception>
#include <memory>
#include <optional>
#include <utility>
#include <unordered_map>
#include <seastar/core/future.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/shared_ptr.hh>

#include "seastarx.hh"

template<typename T>
concept Task = requires(T t) {
    { t.run() } -> std::same_as<future<>>;

    // Must be idempotent
    // Called automatically after `t.run()` resolves
    // If called by somebody else, it should result in `t.run()` quitting
    { t.stop() } -> std::same_as<future<>>;
};

template<typename T, typename... Args>
concept AsyncStartable = requires(Args... args) {
    { T::start(std::forward<Args>(args)...) } -> std::same_as<future<lw_shared_ptr<T>>>;
};

template<typename Key, typename T, typename Hasher = std::hash<Key>>
requires Task<T>
class task_map {
private:
    // Invariant: exactly one of the fields is valid / has value / etc.
    //   1. t != nullptr
    //      The task is running and is ready to use
    //   2. starting.valid()
    //      The task is being started (or re-started)
    //   3. stopping.has_value()
    //      The task is being stopped, but nobody has requested creating it again
    struct task_wrapper {
        lw_shared_ptr<T> t;
        shared_future<lw_shared_ptr<T>> starting;
        std::optional<future<>> stopping;
    };

private:
    std::unordered_map<Key, task_wrapper, Hasher> _tasks;

public:
    inline lw_shared_ptr<T> get_running(const Key& k) {
        if (auto it = _tasks.find(k); it != _tasks.end()) {
            return it->second.t;
        }
        return nullptr;
    }

    template<typename F>
    requires requires (F f, const Key& k, lw_shared_ptr<T> t) {{ f(k, t) } -> std::same_as<void>; }
    inline void for_each_running(F&& f) {
        for (auto& p : _tasks) {
            if (p.second.t) {
                f(p.first, p.second.t);
            }
        }
    }

    template<typename F>
    requires requires (F f, const Key& k, lw_shared_ptr<T> t) {{ f(k, t) } -> std::same_as<future<>>; }
    inline future<> parallel_for_each_running(F&& f) {
        return parallel_for_each(_tasks, [&f] (auto& p) {
            if (p.second.t) {
                return f(p.first, p.second.t);
            }
            return make_ready_future<>();
        });
    }

    // Gets or starts a new task under given key.
    // If the task is already being started by somebody else, it will wait for it.
    // The caller must ensure that `args` are valid
    // until the future returned completes.
    template<typename... Args>
    requires AsyncStartable<T, Args...>
    future<lw_shared_ptr<T>> get_or_start(const Key& k, Args&&... args) {
        auto p = _tasks.try_emplace(k);
        auto it = p.first;
        const bool inserted = p.second;
        task_wrapper& task = it->second;

        if (task.t) {
            // State 1
            // The task is present and is running
            return make_ready_future<lw_shared_ptr<T>>(task.t);
        }

        if (task.starting.valid()) {
            // State 2
            // Somebody else started the task, wait for them
            return task.starting.get_future();
        }

        auto f_wait_for_stop = make_ready_future<>();

        if (task.stopping.has_value()) {
            // Leaving state 3
            f_wait_for_stop = std::move(*task.stopping);
            task.stopping.reset();
        } else {
            // Leaving state 0 - the task_wrapper was just inserted
        }

        // Entering state 2
        promise<lw_shared_ptr<T>> p_started;
        task.starting = shared_future<lw_shared_ptr<T>>(p_started.get_future());

        return std::move(f_wait_for_stop).then([this, it, p_started = std::move(p_started), &args...] () mutable {
            return T::start(std::forward<Args>(args)...).then_wrapped([this, it, p_started = std::move(p_started)] (future<lw_shared_ptr<T>> f) mutable {
                if (f.failed()) {
                    // Transition 2 -> 0
                    // Inform everybody who waits for the future about the error
                    // Then remove the task_wrapper from the map
                    auto eptr = f.get_exception();
                    p_started.set_exception(eptr);
                    _tasks.erase(it);
                    return make_exception_future<lw_shared_ptr<T>>(std::move(eptr));
                }

                // Transition 2 -> 1
                // Satisfy the shared future, immediately clear it and then set `t`
                lw_shared_ptr<T> t = f.get();
                p_started.set_value(t);
                it->second.starting = shared_future<lw_shared_ptr<T>>();
                it->second.t = t;

                // TODO: What about exceptional futures here?
                // Waited on indirectly (in `stop()` or `get_or_start()`)
                (void)t->run().finally([this, it] () mutable {
                    // Maybe we should instruct users about not throwing in `run()`
                    // Transition 1 -> 3
                    // Clear `t` and set `stopping`
                    lw_shared_ptr<T> t = std::move(it->second.t);
                    promise<> p_stopped;
                    it->second.stopping = p_stopped.get_future();
                    return t->stop().finally([this, it, t, p_stopped = std::move(p_stopped)] () mutable {
                        if (it->second.stopping.has_value()) {
                            // Transition 3 -> 0
                            // Nobody has took the `stopping` future, which means
                            // that nobody is trying to re-create the task under
                            // this key. We can remove the task from map.
                            _tasks.erase(it);
                        } else {
                            // Somebody took the `stopping` future, which means
                            // there was a transition from 3 -> 2 and they
                            // want to create the task again. DO NOT remove
                            // the task from the map.
                        }

                        p_stopped.set_value();
                    });
                }).handle_exception([] (std::exception_ptr) {});

                return make_ready_future<lw_shared_ptr<T>>(std::move(t));
            });
        });
    }

    future<> stop() {
        return parallel_for_each(_tasks, [this] (auto& p) {
            task_wrapper& task = p.second;

            if (task.t) {
                // State 1
                // We can explicitly request stop
                return task.t->stop();
            }

            if (task.starting.valid()) {
                // State 2
                // We should wait until the task is created, then stop
                return task.starting.get_future().then([this] (lw_shared_ptr<T> t) {
                    return t->stop();
                });
            }

            if (task.stopping.has_value()) {
                // State 3
                // The task is stopping and will remove itself from the map.
                return std::move(*task.stopping);
            }

            // ???
            assert(false);
            return make_ready_future<>();
        }).then([this] {
            assert(_tasks.empty());
        });
    }
};

// TODO: Tests!!!!!!!!
