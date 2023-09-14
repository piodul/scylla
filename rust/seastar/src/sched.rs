/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#[cxx::bridge]
mod ffi {
    #[namespace = "seastar"]
    unsafe extern "C++" {
        include!("seastar/core/preempt.hh");

        /// Checks whether the current task exhausted its time quota and should
        /// yield to the runtime as soon as possible.
        fn need_preempt() -> bool;
    }
}

use std::future::poll_fn;
use std::task::Poll;

pub use ffi::need_preempt;

/// Preempt if the current task quota expired.
///
/// `maybe_yield()` can be used to break a long computation in a
/// coroutine and allow the reactor to preempt its execution. This
/// allows other tasks to gain access to the CPU. If the task quota
/// did not expire, the coroutine continues execution.
///
/// It should be used in long loops that do not contain other `.await`
/// calls.
///
/// TODO: Example
pub async fn maybe_yield() {
    let mut awaited = false;
    poll_fn(|cx| {
        if !awaited && need_preempt() {
            awaited = true;
            cx.waker().wake_by_ref();
            Poll::Pending
        } else {
            Poll::Ready(())
        }
    })
    .await
}

pub async fn yield_now() {
    let mut awaited = false;
    poll_fn(|cx| {
        if !awaited {
            awaited = true;
            cx.waker().wake_by_ref();
            Poll::Pending
        } else {
            Poll::Ready(())
        }
    })
    .await
}
