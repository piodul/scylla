/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

use std::ffi::c_uint;

pub type ShardId = usize;

extern "C" {
    fn seastar_rs_get_shard_count() -> c_uint;
    fn seastar_rs_get_current_shard() -> c_uint;
}

#[inline(always)]
pub fn shard_count() -> usize {
    // SAFETY: `seastar_rs_get_shard_count` is always safe to call.
    // Hopefully, LTO will make this call cheap
    (unsafe { seastar_rs_get_shard_count() }) as usize
}

#[inline(always)]
pub fn this_shard() -> usize {
    // SAFETY: `seastar_rs_get_current_shard` is always safe to call.
    (unsafe { seastar_rs_get_current_shard() }) as usize
}
