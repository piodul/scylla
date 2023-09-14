use std::ffi::c_void;
use std::marker::PhantomData;
use std::mem::MaybeUninit;

use crate::exception::CxxExceptionPtr;
use crate::future::BoxFutureTarget;
use crate::BoxFuture;

/// Represents a __boxed__ seastar promise, i.e. passed behind a `std::unique_ptr`.
#[repr(C)]
pub struct BoxPromise<T>
where
    T: BoxPromiseTarget,
{
    cpp_prom: *mut c_void,
    _phantom: PhantomData<T>,
}

/// A type that implements this can be passed through a Promise<>.
///
/// This trait can be useful in generic code, but it must not be implemented directly.
/// See `./rust/gen.py` if you want to implement support for more types.
pub unsafe trait BoxPromiseTarget: BoxFutureTarget {
    #[doc(hidden)]
    unsafe fn new() -> *mut c_void;
    #[doc(hidden)]
    unsafe fn set_value(cpp_prom: *const c_void, val: *mut c_void);
    #[doc(hidden)]
    unsafe fn set_exception(cpp_prom: *const c_void, eptr: *mut c_void);
    #[doc(hidden)]
    unsafe fn get_future(cpp_prom: *const c_void) -> *mut c_void;
    #[doc(hidden)]
    unsafe fn free(cpp_prom: *mut c_void);
}

impl<T> BoxPromise<T>
where
    T: BoxPromiseTarget,
{
    /// Creates a new empty promise.
    pub fn new() -> Self {
        Self {
            cpp_prom: unsafe { <T as BoxPromiseTarget>::new() },
            _phantom: PhantomData,
        }
    }

    /// Sets the value of the promise.
    /// Aborts if the promise was already set.
    pub fn set_value(&self, val: T) {
        let mut val_holder = MaybeUninit::new(val);
        unsafe {
            <T as BoxPromiseTarget>::set_value(
                self.cpp_prom as *const c_void,
                &mut val_holder as *mut _ as *mut c_void,
            );
        }
    }

    /// Sets the promise to given exception.
    /// Aborts if the promise was already set.
    pub fn set_exception(&self, eptr: CxxExceptionPtr) {
        let mut eptr_holder = MaybeUninit::new(eptr);
        unsafe {
            <T as BoxPromiseTarget>::set_exception(
                self.cpp_prom as *const c_void,
                &mut eptr_holder as *mut _ as *mut c_void,
            );
        }
    }

    /// Creates a promise from given future.
    /// Aborts if a future was already created from this promise.
    pub fn get_future(&self) -> BoxFuture<T> {
        let cpp_fut = unsafe { <T as BoxPromiseTarget>::get_future(self.cpp_prom) };
        unsafe { BoxFuture::new_from_raw(cpp_fut) }
    }
}

impl<T> Drop for BoxPromise<T>
where
    T: BoxPromiseTarget,
{
    fn drop(&mut self) {
        unsafe {
            <T as BoxPromiseTarget>::free(self.cpp_prom);
        }
    }
}
