/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <memory>
#include <seastar/core/future.hh>

namespace seastar::rs {

/// A glorified std::unique_ptr<seastar::promise<T>>.
template<typename T>
struct promise_box : public std::unique_ptr<seastar::promise<T>> {
public:
    // Needed by C++ to be able to pass promise_box by value.
    using IsRelocatable = std::true_type;

    using value_type = T;
    using promise_type = seastar::promise<T>;

    promise_box(seastar::promise<T>&& f) : promise_box::unique_ptr(std::make_unique<seastar::promise<T>>(std::move(f))) {}
};

} // namespace seastar::rs
