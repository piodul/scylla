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

/// A glorified std::unique_ptr<seastar::future<T>>.
template<typename T>
struct future_box : public std::unique_ptr<seastar::future<T>> {
public:
    // Needed by C++ to be able to pass future_box by value.
    using IsRelocatable = std::true_type;

    using value_type = T;
    using future_type = seastar::future<T>;

    future_box(seastar::future<T>&& f) : future_box::unique_ptr(std::make_unique<seastar::future<T>>(std::move(f))) {}
};

} // namespace seastar::rs
