/*
 * Copyright (C) 2022-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: (AGPL-3.0-or-later and Apache-2.0)
 */

use std::future::Future;
use std::task::{Context, Poll, Waker, RawWaker, RawWakerVTable};
use std::pin::Pin;

use std::sync::{Arc};
use std::sync::atomic::AtomicBool;

#[cxx::bridge(namespace = "rust")]
mod ffi {
    extern "Rust" {
        type MyFuture;
        fn inc(x: i32) -> i32;
        fn poll_my_future(task: Pin<&mut RustTask>, out: &mut u32) -> bool;
        fn create_rust_future() -> *mut MyFuture;
        unsafe fn delete_rust_future(fut: *mut MyFuture);
    }
    extern "C++" {
        type RustTask;
    }
    unsafe extern "C++" {
        include!("rust_task.hh");

        fn get_fut(self: Pin<&mut RustTask>) -> &mut MyFuture;

        fn wake_rust_task(task: Pin<&mut RustTask>);

        unsafe fn schedule_callback_after_one_second(cb: unsafe fn(*mut MyFuture), data: *mut MyFuture);
    }
}
fn inc(x: i32) -> i32 {
    x + 1
}

pub struct MyFuture {
    running: bool,
    done: bool,
    waker: Option<Waker>,
}

impl Future for MyFuture {
    type Output = u32;
    fn poll(mut self: Pin<&mut Self>, ctx: &mut Context<'_>) -> Poll<Self::Output> {
        if !self.running {
            self.as_mut().running = true;
            self.as_mut().waker = Some(ctx.waker().clone());
            fn callback(x: *mut MyFuture) {
                println!("X is: {:p}", x);
                unsafe {
                    (*x).done = true;
                    (*x).waker.take().map(|w| w.wake());
                }
            }
            unsafe {
                ffi::schedule_callback_after_one_second(callback, self.as_ref().get_ref() as *const MyFuture as *mut MyFuture);
            }
            return Poll::Pending;
        }

        if self.done {
            Poll::Ready(1337)
        } else {
            Poll::Pending
        }
    }
}

pub fn poll_my_future(task: Pin<&mut ffi::RustTask>, out: &mut u32) -> bool {
    let waker = unsafe {
        Waker::from_raw(RawWaker::new(task.as_ref().get_ref() as *const ffi::RustTask as *const (), &WAKER_VTABLE))
    };
    let fut = task.get_fut();
    let mut ctx = Context::from_waker(&waker);
    match Pin::new(fut).poll(&mut ctx) {
        Poll::Pending => false,
        Poll::Ready(x) => {
            *out = x;
            true
        },
    }
}

pub fn create_rust_future() -> *mut MyFuture {
    Box::into_raw(Box::new(MyFuture {
        running: false,
        done: false,
        waker: None,
    }))
}

pub unsafe fn delete_rust_future(fut: *mut MyFuture) {
    let _ = Box::from_raw(fut);
}

static WAKER_VTABLE: RawWakerVTable = RawWakerVTable::new(
    waker_clone,
    waker_wake,
    waker_wake,
    waker_drop,
);

fn waker_clone(data: *const ()) -> RawWaker {
    RawWaker::new(data, &WAKER_VTABLE)
}

unsafe fn waker_wake(data :*const ()) {
    ffi::wake_rust_task(Pin::new_unchecked(&mut *(data as *const ffi::RustTask as *mut ffi::RustTask)));
}

fn waker_drop(_data: *const ()) {

}
