/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

use std::cell::{Cell, RefCell};
use std::collections::LinkedList;
use std::future::Future;
use std::task::{Poll, Waker};

// TODO: Make it intrusive.
// It's doable, but it's very tricky - e.g. Tokio does it.
#[derive(Default)]
pub struct ConditionVariable {
    waiters: RefCell<LinkedList<Waker>>,
}

impl ConditionVariable {
    #[inline]
    pub fn new() -> Self {
        Self::default()
    }

    /// Notify variable and wake up a single waiter, if there is one
    #[inline]
    pub fn signal(&self) {
        if let Some(w) = self.waiters.borrow_mut().pop_front() {
            w.wake();
        }
    }

    /// Notify variable and wake up all waiters
    #[inline]
    pub fn broadcast(&self) {
        let mut waiters = self.waiters.borrow_mut();
        while let Some(w) = waiters.pop_front() {
            w.wake();
        }
    }

    /// Wait until this variable is notified.
    #[inline]
    pub fn wait(&self) -> impl Future<Output = ()> + '_ {
        let polled = Cell::new(false);
        self.wait_until(move || polled.replace(true))
    }

    /// Waits until given condition becomes true.
    #[inline]
    pub fn wait_until<'cv, F>(&'cv self, f: F) -> impl Future<Output = ()> + 'cv
    where
        F: Fn() -> bool + 'cv,
    {
        std::future::poll_fn(move |cx| {
            if !f() {
                self.waiters.borrow_mut().push_back(cx.waker().clone());
                Poll::Pending
            } else {
                Poll::Ready(())
            }
        })
    }

    // TODO: Timer-based, or whatever-based variants
    // Might be easy to implement via future cancellation (think: tokio::timeout),
    // but in order not to leak resources we need to use intrusive list.
}

impl Drop for ConditionVariable {
    #[inline]
    fn drop(&mut self) {
        self.broadcast();
    }
}
