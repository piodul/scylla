use std::ffi::{c_char, c_void};
use std::future::Future;
use std::marker::{PhantomData, PhantomPinned};
use std::mem::{ManuallyDrop, MaybeUninit};
use std::pin::Pin;
use std::task::{Context, Poll, Waker};

// Necessary infrastructure needed by Rust to wait for seastar futures.

// Needed by C++

#[no_mangle]
pub unsafe extern "C" fn seastar_rs_rust_future_wake(ptr_to_waker: *mut c_void) {
    let ptr_to_waker = ptr_to_waker as *mut Option<Waker>;
    let waker_opt = (*ptr_to_waker).take();
    if let Some(waker) = waker_opt {
        waker.wake();
    }
}

// Needed by Rust

#[repr(C)]
union FutureValueContainer<T> {
    value: ManuallyDrop<T>,
    err: *mut c_void, // TODO: This should be exception_ptr, unsuppored by cxx right now
}

// Shared with the C++ side.
#[repr(C)]
pub struct FutureTracker<T> {
    waker_ptr: *mut Option<Waker>, // Used solely by the C++ side
    ref_count: c_char,
    state: c_char,
    value: FutureValueContainer<T>,
}

// TODO: Make it possible to create ready futures
pub unsafe trait SeastarFutureTarget: Sized {
    unsafe fn allocate_tracker(
        fut_ptr: *mut c_void,
        waker_ptr: *mut Option<Waker>,
    ) -> *mut FutureTracker<Self>;
    unsafe fn free_tracker(tracker_ptr: *mut FutureTracker<Self>);
    unsafe fn drop_future(fut_ptr: *mut c_void);
}

pub struct SeastarFutureBase<T>
where
    T: SeastarFutureTarget,
{
    cpp_fut: *mut c_void,
    _phantom_data: PhantomData<T>,
}

impl<T> Drop for SeastarFutureBase<T>
where
    T: SeastarFutureTarget,
{
    fn drop(&mut self) {
        unsafe { <T as SeastarFutureTarget>::drop_future(self.cpp_fut) };
    }
}

// A version of the SeastarFuture, but it can be polled by Rust.
pub struct SeastarPollableFuture<T>
where
    T: SeastarFutureTarget,
{
    cpp_fut: *mut c_void,

    // A tracker object shared with C++.
    tracker: *mut FutureTracker<T>,

    // The waker. Can be accessed by the C++ side via `seastar_rs_rust_future_wake`.
    waker: Option<Waker>,

    // The C++ might point to our waker, so this struct must be pinned.
    _phantom_data: PhantomData<T>,
    _phantom_pinned: PhantomPinned,
}

#[doc(hidden)]
impl<T> SeastarPollableFuture<T>
where
    T: SeastarFutureTarget,
{
    #[inline]
    pub fn new_from_raw(fut: SeastarFutureBase<T>) -> Self {
        Self {
            cpp_fut: fut.cpp_fut,
            tracker: std::ptr::null_mut(),
            waker: None,
            _phantom_data: PhantomData,
            _phantom_pinned: PhantomPinned,
        }
    }
}

impl<T> Future for SeastarPollableFuture<T>
where
    T: SeastarFutureTarget,
{
    type Output = T; // TODO: Exceptions

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        // Safety: we will not move out of self, pinky promise.
        let me = unsafe { self.get_unchecked_mut() };
        if !me.cpp_fut.is_null() {
            // We need to attach to the future.
            me.tracker =
                unsafe { <T as SeastarFutureTarget>::allocate_tracker(me.cpp_fut, &mut me.waker) };
            me.cpp_fut = std::ptr::null_mut();
        }
        let state = unsafe { (*me.tracker).state };
        match state {
            0 => {
                me.waker = Some(cx.waker().clone());
                Poll::Pending
            }
            1 => {
                let value = unsafe { ManuallyDrop::take(&mut (*me.tracker).value.value) };
                unsafe { (*me.tracker).state = 0 }; // Mark as moved out
                Poll::Ready(value)
            }
            _ => {
                panic!("Not ready to receive exceptions yet!!!");
            }
        }
    }
}

impl<T> Drop for SeastarPollableFuture<T>
where
    T: SeastarFutureTarget,
{
    fn drop(&mut self) {
        if !self.cpp_fut.is_null() {
            unsafe { <T as SeastarFutureTarget>::drop_future(self.cpp_fut) };
        } else if !self.tracker.is_null() {
            unsafe { <T as SeastarFutureTarget>::free_tracker(self.tracker) };
        }
    }
}

// TODO: Hygiene
macro_rules! define_cpp_future {
    ($rust_name:ident, $cpp_name:ty, $typ:ty) => {
        #[repr(transparent)]
        pub struct $rust_name($crate::future::cpp::SeastarFutureBase<$typ>);

        unsafe impl $crate::future::cpp::SeastarFutureTarget for $typ {
            unsafe fn allocate_tracker(
                fut_ptr: *mut c_void,
                waker_ptr: *mut Option<Waker>,
            ) -> *mut $crate::future::cpp::FutureTracker<Self> {
                extern "C" {
                    #[link_name = concat!("seastar_rs_cpp_shim_", stringify!($cpp_name), "_allocate_tracker")]
                    fn impl_fn(fut_ptr: *mut c_void, waker_ptr: *mut Option<Waker>) -> *mut $crate::future::cpp::FutureTracker<$typ>;
                }
                impl_fn(fut_ptr, waker_ptr)
            }

            unsafe fn free_tracker(tracker_ptr: *mut $crate::future::cpp::FutureTracker<Self>) {
                extern "C" {
                    #[link_name = concat!("seastar_rs_cpp_shim_", stringify!($cpp_name), "_free_tracker")]
                    fn impl_fn(tracker_ptr: *mut $crate::future::cpp::FutureTracker<$typ>);
                }
                impl_fn(tracker_ptr)
            }

            unsafe fn drop_future(fut_ptr: *mut c_void) {
                extern "C" {
                    #[link_name = concat!("seastar_rs_cpp_shim_", stringify!($cpp_name), "_drop_future")]
                    fn impl_fn(fut_ptr: *mut c_void);
                }
                impl_fn(fut_ptr)
            }
        }

        impl ::std::future::IntoFuture for $rust_name {
            type IntoFuture = $crate::future::cpp::SeastarPollableFuture<$typ>;
            type Output = $typ;

            fn into_future(self) -> Self::IntoFuture {
                SeastarPollableFuture::new_from_raw(self.0)
            }
        }

        unsafe impl ::cxx::ExternType for $rust_name {
            type Id = ::cxx::type_id!($cpp_name);
            type Kind = ::cxx::kind::Trivial;
        }
    };
}

pub struct SeastarPromiseBase<T>
where
    T: SeastarPromiseTarget,
{
    cpp_prom: *mut c_void,
    _phantom_data: PhantomData<T>,
}

impl<T> Drop for SeastarPromiseBase<T>
where
    T: SeastarPromiseTarget,
{
    fn drop(&mut self) {
        unsafe { <T as SeastarPromiseTarget>::drop_promise(self.cpp_prom) };
    }
}

pub unsafe trait SeastarPromiseTarget: Sized {
    unsafe fn new() -> *mut c_void;
    unsafe fn set_value(prom_ptr: *mut c_void, v_ptr: *mut MaybeUninit<Self>);
    unsafe fn drop_promise(prom_ptr: *mut c_void);
}

macro_rules! define_cpp_promise {
    ($rust_name:ident, $cpp_name:ty, $typ:ty) => {
        #[repr(transparent)]
        pub struct $rust_name($crate::future::cpp::SeastarPromiseBase<$typ>);

        unsafe impl $crate::future::cpp::SeastarPromiseTarget for $typ {
            unsafe fn new() -> *mut c_void {
                extern "C" {
                    #[link_name = concat!("seastar_rs_cpp_shim_", stringify!($cpp_name), "_new")]
                    fn impl_fn() -> *mut c_void;
                }
                impl_fn()
            }

            unsafe fn set_value(prom_ptr: *mut c_void, v_ptr: *mut MaybeUninit<Self>) {
                extern "C" {
                    #[link_name = concat!("seastar_rs_cpp_shim_", stringify!($cpp_name), "_set_value")]
                    fn impl_fn(prom_ptr: *mut c_void, v_ptr: *mut MaybeUninit<$typ>);
                }
                impl_fn(prom_ptr, v_ptr)
            }

            unsafe fn drop_promise(prom_ptr: *mut c_void) {
                extern "C" {
                    #[link_name = concat!("seastar_rs_cpp_shim_", stringify!($cpp_name), "_drop_promise")]
                    fn impl_fn(prom_ptr: *mut c_void);
                }
                impl_fn(prom_ptr)
            }
        }

        impl $rust_name {
            pub fn new() -> Self {
                Self(SeastarPromiseBase {
                    cpp_prom: unsafe { <$typ as SeastarPromiseTarget>::new() },
                    _phantom_data: PhantomData,
                })
            }

            pub fn set_value(&mut self, v: $typ) {
                let mut v = std::mem::MaybeUninit::new(v);
                unsafe {
                    <$typ as SeastarPromiseTarget>::set_value(
                        self.0.cpp_prom,
                        (&mut v) as *mut MaybeUninit<$typ>,
                    )
                }
            }
        }
    }
}

define_cpp_future!(CppBoolFuture, future_bool, bool);
define_cpp_future!(CppU32Future, future_u32, u32);

define_cpp_promise!(CppBoolPromise, promise_bool, bool);
define_cpp_promise!(CppU32Promise, promise_u32, u32);
