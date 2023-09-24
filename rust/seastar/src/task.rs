/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

use std::ffi::{c_int, c_void};
use std::future::Future;
use std::panic::AssertUnwindSafe;
use std::pin::Pin;
use std::task::{Context, Poll, RawWaker, RawWakerVTable, Waker};

use crate::exception::CxxExceptionPtr;
use crate::future::BoxFutureTarget;
use crate::promise::{BoxPromise, BoxPromiseTarget};
use crate::BoxFuture;

pub fn spawn_for_cpp<T>(future: impl Future<Output = T> + 'static) -> BoxFuture<T>
where
    T: BoxPromiseTarget + BoxFutureTarget + 'static,
{
    let promise = BoxPromise::new();
    let sfut = promise.get_future();
    spawn_void(SpawnFuture { future, promise });
    sfut
}

// A future that forwards the result of the inner future to the seastar promise
struct SpawnFuture<F, T>
where
    F: Future<Output = T>,
    T: BoxPromiseTarget,
{
    future: F,
    promise: BoxPromise<T>,
}

impl<F, T> SpawnFuture<F, T>
where
    F: Future<Output = T>,
    T: BoxPromiseTarget,
{
    fn get_inner(self: Pin<&mut Self>) -> Pin<&mut F> {
        unsafe { self.map_unchecked_mut(|s| &mut s.future) }
    }

    fn get_promise(self: Pin<&mut Self>) -> &mut BoxPromise<T> {
        unsafe { &mut self.get_unchecked_mut().promise }
    }
}

impl<F, T> Future for SpawnFuture<F, T>
where
    F: Future<Output = T>,
    T: BoxPromiseTarget,
{
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.as_mut();
        let poll_result = std::panic::catch_unwind(AssertUnwindSafe(|| me.get_inner().poll(cx)));
        match poll_result {
            Ok(Poll::Pending) => Poll::Pending,
            Ok(Poll::Ready(t)) => {
                self.get_promise().set_value(t);
                Poll::Ready(())
            }
            Err(payload) => {
                let eptr = CxxExceptionPtr::try_from_panic(payload)
                    .unwrap_or_else(|payload| CxxExceptionPtr::panic_to_exception(payload));
                self.get_promise().set_exception(eptr);
                Poll::Ready(())
            }
        }
    }
}

// TODO: Provide a version of spawn which works within rust and doesn't need the BoxFuture/BoxPromise shenanigans

type FuturePollFn = extern "C" fn(task: *mut c_void, fut: *mut c_void) -> c_int;

/// Spawns a task that polls a future which doesn't return anything.
/// The task is not being waited on, it is the responsibility of the future
/// being polled to synchronize with waiters.
fn spawn_void<Fut>(fut: Fut)
where
    Fut: Future<Output = ()> + 'static,
{
    extern "C" {
        #[link_name = "seastar_rs_task_spawn"]
        fn impl_fn(poll_fn: FuturePollFn, fut: *mut c_void);
    }
    let fut_holder = Box::new(fut);
    unsafe {
        impl_fn(
            poll_fn::<Fut>,
            Box::into_raw(fut_holder) as *mut _ as *mut _,
        );
    }
}

extern "C" fn poll_fn<Fut>(task: *mut c_void, fut: *mut c_void) -> c_int
where
    Fut: Future<Output = ()>,
{
    let raw_waker = RawWaker::new(task as *const _ as *const (), &WAKER_VTABLE);
    let waker = unsafe { Waker::from_raw(raw_waker) };
    let mut context = Context::from_waker(&waker);

    let fut_ref = unsafe { &mut *(fut as *mut Fut) };
    let fut_pin_ref = unsafe { Pin::new_unchecked(fut_ref) };

    let status = match fut_pin_ref.poll(&mut context) {
        Poll::Pending => 0,
        Poll::Ready(()) => {
            // Drop the Rust future
            let _ = unsafe { Box::from_raw(fut as *mut Fut) };
            1
        }
    };

    // Waker::from_raw does not increase the reference count. On the other hand, its Drop impl does.
    // Therefore we must prevent the destructor from running (no resources are being leaked).
    std::mem::forget(waker);

    status
}

static WAKER_VTABLE: RawWakerVTable =
    RawWakerVTable::new(waker_clone, waker_wake, waker_wake_by_ref, waker_drop);

unsafe fn waker_clone(ptr: *const ()) -> RawWaker {
    extern "C" {
        #[link_name = "seastar_rs_waker_clone"]
        fn impl_fn(data: *const ());
    }
    impl_fn(ptr);
    RawWaker::new(ptr, &WAKER_VTABLE)
}

unsafe fn waker_wake(ptr: *const ()) {
    extern "C" {
        #[link_name = "seastar_rs_waker_wake"]
        fn impl_fn(data: *const ());
    }
    impl_fn(ptr);
}

unsafe fn waker_wake_by_ref(ptr: *const ()) {
    extern "C" {
        #[link_name = "seastar_rs_waker_wake_by_ref"]
        fn impl_fn(data: *const ());
    }
    impl_fn(ptr);
}

unsafe fn waker_drop(ptr: *const ()) {
    extern "C" {
        #[link_name = "seastar_rs_waker_drop"]
        fn impl_fn(data: *const ());
    }
    impl_fn(ptr);
}
