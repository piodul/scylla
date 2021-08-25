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

#include <cassert>
#include <concepts>
#include <exception>
#include <memory>
#include <optional>
#include <utility>
#include <unordered_map>
#include <seastar/core/future.hh>
#include <seastar/core/shared_future.hh>

#include "seastarx.hh"

template<typename T, typename... Args>
concept IsAsyncStartable = requires(Args... args) {
    { T::start(std::forward(args)...) } -> std::convertible_to<future<std::unique_ptr<T>>>;
};

template<typename T>
concept IsAsyncStoppable = requires(T t) {
    { t.stop() } -> std::convertible_to<future<>>;
};

template<typename Key, typename Task>
requires IsAsyncStoppable<Task>
class task_map {
private:
    std::unordered_map<Key, std::unique_ptr<Task>> _running_tasks;
    std::unordered_map<Key, shared_future<>> _starting_tasks;
    std::unordered_map<Key, shared_future<>> _stopping_tasks;

public:
    Task* get_task(const Key& k) {
        if (auto it = _running_tasks.find(k); it == _running_tasks.end()) {
            return it->second.get();
        }
        return nullptr;
    }

    // Starts a new task under given key.
    // If the task is already being started by somebody else, it will wait for it.
    // The caller must ensure that all references passed in `args` are valid
    // until the future returned completes.
    // Because there is no guarantee that the task won't be stopped
    template<typename... Args>
    requires IsAsyncStartable<Task, Args...>
    future<> start_task(const Key& k, Args&&... args) {
        // TODO: Should we use assert here?
        assert(!_running_tasks.contains(k));

        // If the task is already starting, wait for it
        if (auto it = _starting_tasks.find(k); it != _starting_tasks.end()) {
            return it->second.get_future();
        }

        // If the task under this key is being stopped, wait for it
        // and only then start a new task
        if (auto it = _stopping_tasks.find(k); it != _stopping_tasks.end()) {
            return it->second.get_future();
        }

        // Insert a new shared future to into _starting_tasks, start waiting
        // and satisfy the shared future after we complete initialization
        // (successful or not)
        auto it = _starting_tasks.emplace(k, shared_future<>(Task::start(std::forward<Args>(args)...)));
        return it->second.get_future();
    }

    future<> stop() {
        // TODO
        return make_ready_future<>();
    }


};

