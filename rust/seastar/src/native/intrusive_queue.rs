/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

use std::cell::Cell;
use std::marker::{PhantomData, PhantomPinned};
use std::ptr::NonNull;

pub unsafe trait IntrusiveQueueElement {
    fn get_link(&self) -> &Link<Self>;
}

#[derive(Debug)]
pub struct IntrusiveQueue<T: IntrusiveQueueElement + ?Sized> {
    first: Cell<Option<NonNull<T>>>,
    last: Cell<Option<NonNull<T>>>,
    _phantom: PhantomData<T>,
    _pinned: PhantomPinned,
}

impl<T: IntrusiveQueueElement + ?Sized> Default for IntrusiveQueue<T> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T: IntrusiveQueueElement + ?Sized> IntrusiveQueue<T> {
    /// Creates an empty list.
    pub const fn new() -> Self {
        Self {
            first: Cell::new(None),
            last: Cell::new(None),
            _phantom: PhantomData,
            _pinned: PhantomPinned,
        }
    }

    /// Inserts a new element to the list.
    ///
    /// # Safety
    ///
    /// The linked list is in consistent state.
    pub unsafe fn push_back(&self, t: &T) {
        let t_link = t.get_link();

        assert!(t_link.queue.get().is_none());

        let old_last = self.last.get();

        t_link.queue.set(Some(self.into()));
        t_link.prev.set(old_last);
        t_link.next.set(None);

        if let Some(old_last) = old_last {
            unsafe {
                let old_last_link = old_last.as_ref().get_link();
                old_last_link.next.set(Some(t.into()));
            }
        } else {
            self.first.set(Some(t.into()));
        }

        self.last.set(Some(t.into()));
    }

    /// Pops an element from the front of the queue.
    ///
    /// # Safety
    ///
    /// The linked list is in consistent state.
    pub unsafe fn pop_front(&self) -> Option<&T> {
        if let Some(old_first) = self.first.get() {
            unsafe {
                let old_first = old_first.as_ref();
                let old_first_link = old_first.get_link();
                let new_first = old_first_link.next.get();

                old_first_link.queue.set(None);
                old_first_link.prev.set(None);
                old_first_link.next.set(None);

                self.first.set(new_first);
                if new_first.is_none() {
                    self.last.set(None);
                }

                Some(old_first)
            }
        } else {
            None
        }
    }
}

#[derive(Debug)]
pub struct Link<T: IntrusiveQueueElement + ?Sized> {
    queue: Cell<Option<NonNull<IntrusiveQueue<T>>>>,
    prev: Cell<Option<NonNull<T>>>,
    next: Cell<Option<NonNull<T>>>,
    _phantom: PhantomData<T>,
    _pinned: PhantomPinned,
}

impl<T: IntrusiveQueueElement + ?Sized> Default for Link<T> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T: IntrusiveQueueElement + ?Sized> Link<T> {
    pub const fn new() -> Self {
        Self {
            queue: Cell::new(None),
            prev: Cell::new(None),
            next: Cell::new(None),
            _phantom: PhantomData,
            _pinned: PhantomPinned,
        }
    }

    pub fn is_linked(&self) -> bool {
        self.queue.get().is_some()
    }

    /// # Safety
    ///
    /// If the link is a part of a list, it must be in consistent state.
    pub unsafe fn unlink(&self) {
        if let Some(queue) = self.queue.get() {
            if let Some(prev) = self.prev.get() {
                prev.as_ref().get_link().next.set(self.next.get());
                self.prev.set(None);
            } else {
                queue.as_ref().first.set(None);
            }
            if let Some(next) = self.next.get() {
                next.as_ref().get_link().prev.set(self.prev.get());
                self.next.set(None);
            } else {
                queue.as_ref().last.set(None);
            }
            self.queue.set(None);
        }
    }
}

struct Pointers<T> {
    prev: Option<NonNull<Link<T>>>,
    next: Option<NonNull<Link<T>>>,
}
