/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <seastar/core/smp.hh>

extern "C" unsigned int seastar_rs_get_shard_count() {
    return seastar::smp::count;
}

extern "C" unsigned int seastar_rs_get_current_shard() {
    return seastar::this_shard_id();
}
