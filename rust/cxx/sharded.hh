/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include "rust/cxx.h"

#include <seastar/core/sharded.hh>

namespace seastar::rs {

// A sharded service, written in rust.
template<typename S>
struct sharded {
private:
    void* _handle;

public:
    sharded(sharded&&) = delete;
    sharded(const sharded&) = delete;

    S& local();
    const S& local() const;

    future<> stop();
};


} // namespace seastar::rs
