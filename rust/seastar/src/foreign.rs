/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

use std::fmt::Display;
use std::future::Future;
use std::mem::MaybeUninit;
use std::rc::Rc;

use futures_util::future::Either;

/// Keeps ownership of an object which maybe was constructed on a different shard.
pub struct ForeignCell<T: 'static> {
    cell: MaybeUninit<T>,
    origin_shard: usize,
}

impl<T: 'static> ForeignCell<T> {
    #[inline]
    pub fn new(v: T) -> Self {
        Self {
            cell: MaybeUninit::new(v),
            origin_shard: crate::smp::this_shard(),
        }
    }

    #[inline]
    pub fn origin_shard(&self) -> usize {
        self.origin_shard
    }

    #[inline]
    pub fn try_get(&self) -> Result<&T, NotInOriginShard> {
        if crate::smp::this_shard() == self.origin_shard {
            // SAFETY:
            // - We just checked that this call happens on the original shard
            // - The value inside the cell is valid because we are only moving it out
            //   in try_unwrap and Drop
            Ok(unsafe { self.cell.assume_init_ref() })
        } else {
            Err(NotInOriginShard)
        }
    }

    #[inline]
    pub fn try_unwrap(self) -> Result<T, Self> {
        if crate::smp::this_shard() == self.origin_shard {
            // SAFETY:
            // - We just checked that this call happens on the original shard
            // - The value inside the cell is valid because we are only moving it out
            //   in try_unwrap and Drop
            let value = unsafe { self.cell.assume_init_read() };

            // Prevent the Drop impl from running so that we don't drop the value twice
            std::mem::forget(self);

            Ok(value)
        } else {
            Err(self)
        }
    }

    /// Destroys the held value on the correct shard and waits until it is destroyed.
    ///
    /// The returned future can be used to wait for completion, but it's not required to poll it.
    #[inline]
    pub async fn drop_async(mut self) {
        let f = unsafe { self.drop_async_in_place() };

        // Prevent the Drop impl from running so that we don't drop the value twice.
        // Do it now before we start polling the future.
        std::mem::forget(self);

        f.await;
    }

    /// Destroys the held value on the correct shard and waits until it is destroyed.
    ///
    /// The returned future can be used to wait for completion, but it's not required to poll it.
    ///
    /// # Safety
    ///
    /// After this operation, this ForeignCell's Drop must not be run. Pass self to std::mem::forget afterwards.
    /// These semantics exist for the sake of Drop itself, which gives access to self through mutable reference.
    unsafe fn drop_async_in_place(&mut self) -> impl Future<Output = ()> {
        struct AssertSendCell<U>(MaybeUninit<U>);
        unsafe impl<U> Send for AssertSendCell<U> {}
        impl<U> AssertSendCell<U> {
            unsafe fn drop_contents(&mut self) {
                self.0.assume_init_drop();
            }
        }

        if crate::smp::this_shard() == self.origin_shard {
            // SAFETY:
            // - The value inside the cell is valid
            // - We are on the correct thread and we can drop it here
            unsafe { self.cell.assume_init_drop() };

            Either::Left(async {})
        } else {
            let cell = std::mem::replace(&mut self.cell, MaybeUninit::uninit());
            let mut cell = AssertSendCell(cell);
            let origin_shard = self.origin_shard;

            Either::Right(async move {
                // We do not need to check the result - program will abort is destructor panics.
                let _ = crate::task::submit_to(origin_shard, move || {
                    // SAFETY:
                    // - The value inside the cell is valid
                    // - We are on the correct thread and we can drop it here
                    unsafe {
                        cell.drop_contents();
                    }
                    // For some reason, the code below doesn't work because the whole closure becomes !Send.
                    // I don't know why, maybe it's a compiler bug. In any case, the `drop_contents` method
                    // does the trick.
                    // unsafe {
                    //     cell.0.assume_init_drop();
                    // }

                    // The closure must return a future
                    async {}
                })
                .await;
            })
        }
    }
}

impl<T: Sync + 'static> ForeignCell<T> {
    #[inline]
    pub fn get(&self) -> &T {
        // SAFETY:
        // - T is Sync and therefore can be accessed from any shard
        // - The value inside the cell is valid because we are only moving it out
        //   in try_unwrap and Drop
        unsafe { self.cell.assume_init_ref() }
    }
}

impl<T: Sync + 'static> ForeignCell<Rc<T>> {
    #[inline]
    pub fn get_deref(&self) -> &T {
        // SAFETY:
        // - Although Rc itself is not Sync, T is - we do not touch the reference counts
        //   but only return a reference to the value inside
        // - The value inside the cell is valid because we are only moving it out
        //   in try_unwrap and Drop
        unsafe { self.cell.assume_init_ref().as_ref() }
    }
}

impl<T: 'static> Drop for ForeignCell<T> {
    fn drop(&mut self) {
        let _ = unsafe { self.drop_async_in_place() };
    }
}

// SAFETY:
// TODO
unsafe impl<T> Send for ForeignCell<T> {}
unsafe impl<T> Sync for ForeignCell<T> {}

#[derive(Debug)]
pub struct NotInOriginShard;

impl Display for NotInOriginShard {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("not in origin shard")
    }
}
