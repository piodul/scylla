/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

use std::ffi::c_void;
use std::future::{Future, IntoFuture};
use std::marker::PhantomData;
use std::mem::{ManuallyDrop, MaybeUninit};
use std::pin::Pin;
use std::task::{Context, Poll, Waker};

use crate::exception::{CxxExceptionPtr, Result};

/// Represents a __boxed__ seastar future, i.e. passed behind a `std::unique_ptr`.
#[repr(C)]
pub struct BoxFuture<T>
where
    T: BoxFutureTarget,
{
    cpp_fut: *mut c_void,
    _phantom: PhantomData<T>,
}

/// A type that implements this can be passed through a SeastarFuture<>.
///
/// This trait can be useful in generic code, but it must not be implemented directly.
/// See `./rust/gen.py` if you want to implement support for more types.
pub unsafe trait BoxFutureTarget {
    #[doc(hidden)]
    unsafe fn make_ready_future(val: *mut c_void) -> *mut c_void;
    #[doc(hidden)]
    unsafe fn make_exception_future(eptr: *mut c_void) -> *mut c_void;
    #[doc(hidden)]
    unsafe fn free(cpp_fut: *mut c_void);
    #[doc(hidden)]
    unsafe fn attach_poll_state(cpp_fut: *mut c_void, poll_state: *mut c_void);
}

impl<T> BoxFuture<T>
where
    T: BoxFutureTarget,
{
    pub(crate) unsafe fn new_from_raw(cpp_fut: *mut c_void) -> Self {
        Self {
            cpp_fut,
            _phantom: PhantomData,
        }
    }
}

/// Creates a new seastar future that is immediately ready.
pub fn make_ready_future<T>(val: T) -> BoxFuture<T>
where
    T: BoxFutureTarget,
{
    let mut val_holder = MaybeUninit::new(val);
    BoxFuture {
        cpp_fut: unsafe {
            <T as BoxFutureTarget>::make_ready_future(&mut val_holder as *mut _ as *mut c_void)
        },
        _phantom: PhantomData,
    }
}

/// Creates a new seastar future that contains an exception.
pub fn make_exception_future<T>(eptr: CxxExceptionPtr) -> BoxFuture<T>
where
    T: BoxFutureTarget,
{
    let mut eptr_holder = MaybeUninit::new(eptr);
    BoxFuture {
        cpp_fut: unsafe {
            <T as BoxFutureTarget>::make_exception_future(&mut eptr_holder as *mut _ as *mut c_void)
        },
        _phantom: PhantomData,
    }
}

impl<T> IntoFuture for BoxFuture<T>
where
    T: BoxFutureTarget,
{
    type IntoFuture = BoxFuturePoller<T>;
    type Output = Result<T>;

    fn into_future(self) -> Self::IntoFuture {
        let poll_state: *mut FuturePollAndWaker<T> = Box::into_raw(Box::new(FuturePollAndWaker {
            poll: FuturePoll {
                discr: FuturePollDiscriminant::Pending,
                ref_count: 2,
                state: FuturePollState { empty: () },
            },
            waker: None,
        }));
        unsafe {
            <T as BoxFutureTarget>::attach_poll_state(self.cpp_fut, poll_state as *mut c_void);
        }
        BoxFuturePoller(poll_state)
    }
}

impl<T> Drop for BoxFuture<T>
where
    T: BoxFutureTarget,
{
    fn drop(&mut self) {
        unsafe {
            <T as BoxFutureTarget>::free(self.cpp_fut);
        }
    }
}

/// A _Rust_ future which wraps a seastar::future and is able to poll it.
pub struct BoxFuturePoller<T>(*mut FuturePollAndWaker<T>)
where
    T: BoxFutureTarget;

impl<T> Drop for BoxFuturePoller<T>
where
    T: BoxFutureTarget,
{
    fn drop(&mut self) {
        unsafe {
            let new_count = (*self.0).poll.ref_count - 1;
            if new_count == 0 {
                let _ = Box::from_raw(self.0);
            } else {
                (*self.0).poll.ref_count = new_count;
            }
        }
    }
}

impl<T> Future for BoxFuturePoller<T>
where
    T: BoxFutureTarget,
{
    type Output = Result<T>;
    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let discr = unsafe { (*self.0).poll.discr };
        match discr {
            FuturePollDiscriminant::Pending => {
                let waker = unsafe { &mut (*self.0).waker };
                match waker {
                    Some(old_waker) if cx.waker().will_wake(&old_waker) => {}
                    _ => {
                        *waker = Some(cx.waker().clone());
                    }
                }
                Poll::Pending
            }
            FuturePollDiscriminant::Value => unsafe {
                let value = ManuallyDrop::take(&mut (*self.0).poll.state.value);
                (*self.0).poll.discr = FuturePollDiscriminant::MovedOut;
                Poll::Ready(Ok(value))
            },
            FuturePollDiscriminant::Exception => unsafe {
                let eptr = ManuallyDrop::take(&mut (*self.0).poll.state.eptr);
                (*self.0).poll.discr = FuturePollDiscriminant::MovedOut;
                Poll::Ready(Err(eptr))
            },
            FuturePollDiscriminant::MovedOut => {
                panic!("Tried to poll a future that was already polled to completion");
            }
        }
    }
}

#[repr(u8)]
#[derive(Copy, Clone)]
enum FuturePollDiscriminant {
    Pending = 0,
    #[allow(unused)] // only set by c++
    Value = 1,
    #[allow(unused)] // only set by c++
    Exception = 2,
    MovedOut = 3,
}

#[repr(C)]
union FuturePollState<T> {
    empty: (),
    value: ManuallyDrop<T>,
    eptr: ManuallyDrop<CxxExceptionPtr>,
}

#[repr(C)]
struct FuturePoll<T> {
    discr: FuturePollDiscriminant,
    ref_count: u8,
    state: FuturePollState<T>,
}

#[repr(C)]
struct FuturePollAndWaker<T> {
    poll: FuturePoll<T>,
    waker: Option<Waker>,
}

#[doc(hidden)]
pub mod internal {
    use std::ffi::c_void;

    use super::FuturePollAndWaker;

    pub unsafe fn future_poll_dispose<T>(future_poll_ptr: *mut c_void) {
        let _ = Box::from_raw(future_poll_ptr as *mut FuturePollAndWaker<T>);
    }

    pub unsafe fn future_poll_wake<T>(future_poll_ptr: *mut c_void) {
        let fpaw = &mut *(future_poll_ptr as *mut FuturePollAndWaker<T>);
        if let Some(waker) = fpaw.waker.take() {
            waker.wake();
        }
    }
}
