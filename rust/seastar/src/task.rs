/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

use std::any::Any;
use std::cell::Cell;
use std::ffi::{c_int, c_uint, c_void};
use std::future::Future;
use std::mem::MaybeUninit;
use std::panic::AssertUnwindSafe;
use std::pin::Pin;
use std::rc::Rc;
use std::task::{Context, Poll, RawWaker, RawWakerVTable, Waker};

use crate::exception::CxxExceptionPtr;
use crate::future::BoxFutureTarget;
use crate::native::oneshot::{self, OneshotCell};
use crate::promise::{BoxPromise, BoxPromiseTarget};
use crate::smp::{self, ShardId};
use crate::BoxFuture;

// TODO: Use pin_project to reduce reliance on unsafe code

/// Spawns a task and returns a BoxFuture<T> that returns its result.
///
/// Use this function if you want to spawn an asynchronous operation which will be waited on by C++.
/// If you just want to spawn a Rust task and also wait on it from Rust, consider using `spawn` instead
/// which doesn't require code generation.
pub fn spawn_for_cpp<T>(future: impl Future<Output = T> + 'static) -> BoxFuture<T>
where
    T: BoxPromiseTarget + BoxFutureTarget + 'static,
{
    let promise = BoxPromise::new();
    let sfut = promise.get_future();
    let completer = move |res| match res {
        Ok(t) => promise.set_value(t),
        Err(payload) => {
            let eptr = CxxExceptionPtr::try_from_panic(payload)
                .unwrap_or_else(|payload| CxxExceptionPtr::panic_to_exception(payload));
            promise.set_exception(eptr);
        }
    };
    spawn_with_completer(future, completer);
    sfut
}

/// Spawns a task and returns a Rust future that can be used to wait on that task.
pub fn spawn<T>(future: impl Future<Output = T> + 'static) -> impl Future<Output = T>
where
    T: 'static,
{
    let (sender, receiver) = oneshot::oneshot();
    let completer = move |res| match sender.send(res) {
        Ok(()) => {}
        Err(_) => {
            // TODO: Better errors
            eprintln!("exceptional future ignored!");
        }
    };
    spawn_with_completer(future, completer);
    async move {
        match receiver.await.unwrap() {
            Ok(v) => v,
            Err(payload) => std::panic::resume_unwind(payload),
        }
    }
}

// Returns a future
type SubmitToCallFn = extern "C" fn(data: *mut c_void) -> *mut c_void;
type SubmitToCleanupFn = extern "C" fn(data: *mut c_void);

pub fn submit_to<T, F, Fun>(target_shard: ShardId, f: Fun) -> impl Future<Output = T>
where
    Fun: FnOnce() -> F + Send + 'static,
    F: Future<Output = T>,
    T: Send + 'static,
{
    assert!(target_shard < smp::shard_count());

    extern "C" {
        #[link_name = "seastar_rs_task_submit_to"]
        fn impl_fn(
            call_fn: SubmitToCallFn,
            cleanup_fn: SubmitToCleanupFn,
            data: *mut c_void,
            shard: c_uint,
        );
    }

    // Create a context object that will be shared between this thread and the other one.
    // All synchronization is being done by seastar futures.
    // The context is owned both by the `poll_fn` future and the fiber spawned by `submit_to`.
    let ctx = Rc::new(SpawnRemoteRustContext {
        cell: OneshotCell::new(),
        value: Cell::new(None),
        f: Cell::new(MaybeUninit::new(f)),
    });
    let ctx2 = Rc::into_raw(Rc::clone(&ctx)) as *mut SpawnRemoteRustContext<T, Fun> as *mut c_void;

    // Call seastar::smp::submit_to. It will call `submit_to_call_fn` function on the remote shard, which will call
    // the `f` function and create a task to poll it. After the task is polled to completion, `submit_to_cleanup_fn`
    // will be called, but on the original shard - the latter will release the ownership of the shared context.
    unsafe {
        impl_fn(
            submit_to_call_fn::<T, F, Fun>,
            submit_to_cleanup_fn::<T, Fun>,
            ctx2,
            target_shard as c_uint,
        );
    }

    std::future::poll_fn(move |cx| match ctx.cell.poll_recv(cx) {
        Poll::Pending => Poll::Pending,
        Poll::Ready(Ok(())) => {
            let v = ctx
                .value
                .take()
                .expect("submit_to future polled after it was closed");
            match v {
                Ok(v) => Poll::Ready(v),
                Err(payload) => std::panic::resume_unwind(payload),
            }
        }
        Poll::Ready(Err(_)) => {
            panic!("submit_to future polled after it was closed");
        }
    })
}

extern "C" fn submit_to_call_fn<T, F, Fun>(data: *mut c_void) -> *mut c_void
where
    Fun: FnOnce() -> F + Send + 'static,
    F: Future<Output = T>,
    T: Send + 'static,
{
    // Safety: cpp side makes sure that `ctx` is a valid pointer.
    let ctx = data as *const SpawnRemoteRustContext<T, Fun>;
    let value_cell = unsafe { &(*ctx).value };

    // Safety: `ctx` is valid.
    // This function is called only once for a given SpawnRemoteRustContext,
    // so `f` was properly initialized before reaching this line.
    let fun = unsafe { (*ctx).f.replace(MaybeUninit::uninit()).assume_init() };

    let prom = BoxPromise::new();
    let sfut = prom.get_future();

    let future = async move { fun().await };
    let completer = move |res| {
        value_cell.set(Some(res));
        prom.set_value(());
    };

    spawn_with_completer(future, completer);

    BoxFuture::into_raw(sfut)
}

extern "C" fn submit_to_cleanup_fn<T, Fun>(data: *mut c_void) {
    let ctx = unsafe { Rc::from_raw(data as *const SpawnRemoteRustContext<T, Fun>) };

    // TODO: Warning about an ignored value
    let _ = ctx.cell.send(());

    // Explicitly drop the Rc to the context, decrementing its reference count
    std::mem::drop(ctx);
}

/// A structure that contains data relevant to a submit_to call.
/// It is kept alive by the handle returned from submit_to, and also
/// the `.finally` call in `seastar_rs_task_submit_to`.
struct SpawnRemoteRustContext<T, Fun> {
    /// Used to synchronize on the calling shard.
    cell: OneshotCell<()>,

    /// Written by the remote shard, read by the calling shard.
    /// Synchronization between threads is done by seastar - the local thread
    /// waits for the future returned by smp::submit_to.
    value: Cell<Option<Result<T, Box<dyn Any + Send>>>>,

    /// A function used to create a `Fut` and then `SpawnRemoteRustFuture<T, Fut>`.
    /// Initialzed on the calling shard, consumed on the remote shard.
    f: Cell<MaybeUninit<Fun>>,
}

type FuturePollFn = extern "C" fn(task: *mut c_void, fut: *mut c_void) -> c_int;

/// Spawns a task that polls a future which doesn't return anything.
/// The task is not being waited on, it is the responsibility of the future
/// being polled to synchronize with waiters.
/// TODO: Adjust the comment
fn spawn_with_completer<Fut, T, Completer>(future: Fut, completer: Completer)
where
    Fut: Future<Output = T> + 'static,
    Completer: FnOnce(Result<T, Box<dyn Any + Send + 'static>>),
{
    extern "C" {
        #[link_name = "seastar_rs_task_spawn"]
        fn impl_fn(poll_fn: FuturePollFn, fut: *mut c_void);
    }
    let completer = Some(completer);
    let future = SpawnFuture { future, completer };
    let poller = poll_fn_of(&future);
    let fut_holder = Box::new(future);
    unsafe {
        impl_fn(poller, Box::into_raw(fut_holder) as *mut _ as *mut _);
    }
}

struct SpawnFuture<F, T, C>
where
    F: Future<Output = T>,
    C: FnOnce(Result<T, Box<dyn Any + Send + 'static>>),
{
    future: F,
    completer: Option<C>,
}

impl<F, T, C> SpawnFuture<F, T, C>
where
    F: Future<Output = T>,
    C: FnOnce(Result<T, Box<dyn Any + Send + 'static>>),
{
    fn get_inner(self: Pin<&mut Self>) -> Pin<&mut F> {
        unsafe { self.map_unchecked_mut(|s| &mut s.future) }
    }

    fn invoke_completer(self: Pin<&mut Self>, v: Result<T, Box<dyn Any + Send + 'static>>) {
        let completer = match unsafe { &mut self.get_unchecked_mut().completer }.take() {
            Some(completer) => completer,
            None => {
                eprintln!("fatal error: SpawnFuture polled after completion");
                std::process::abort()
            }
        };
        completer(v);
    }
}

impl<F, T, C> Future for SpawnFuture<F, T, C>
where
    F: Future<Output = T>,
    C: FnOnce(Result<T, Box<dyn Any + Send + 'static>>),
{
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.as_mut();
        let poll_result = std::panic::catch_unwind(AssertUnwindSafe(|| me.get_inner().poll(cx)));
        match poll_result {
            Ok(Poll::Pending) => Poll::Pending,
            Ok(Poll::Ready(t)) => {
                self.invoke_completer(Ok(t));
                Poll::Ready(())
            }
            Err(e) => {
                self.invoke_completer(Err(e));
                Poll::Ready(())
            }
        }
    }
}

const fn poll_fn_of<Fut>(_: &Fut) -> FuturePollFn
where
    Fut: Future<Output = ()>,
{
    poll_fn::<Fut>
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
