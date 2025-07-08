/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

use std::any::Any;
use std::ffi::{c_char, c_void, CStr, CString};
use std::mem::MaybeUninit;

pub type Result<T, E = CxxExceptionPtr> = std::result::Result<T, E>;

pub trait ResultExt {
    type T;
    fn eunwrap(self) -> Self::T;
}

impl<T> ResultExt for Result<T> {
    type T = T;

    #[inline]
    fn eunwrap(self) -> Self::T {
        match self {
            Ok(t) => t,
            Err(eptr) => eptr.rethrow(),
        }
    }
}

/// A `std::exception_ptr`.
// We are only supporting libstd's implementation, so we can assume a lot of things, and verify some on the cpp side.
#[repr(transparent)]
pub struct CxxExceptionPtr {
    data: MaybeUninit<*const c_void>,
}

// std::exception_ptr has std::shared_ptr-like semantics, so it's safe to move it between shards.
unsafe impl Send for CxxExceptionPtr {}
unsafe impl Sync for CxxExceptionPtr {}

impl CxxExceptionPtr {
    #[inline]
    pub const fn null() -> Self {
        // TODO: Should this be initialized by C++?
        Self {
            data: MaybeUninit::new(std::ptr::null_mut()),
        }
    }

    #[inline]
    pub fn is_null(&self) -> bool {
        // TODO: Should this be manipulated from C++?
        unsafe { self.data.assume_init().is_null() }
    }

    #[inline]
    pub fn try_catch<E>(&self) -> Option<&E>
    where
        E: CxxExceptionPtrTarget,
    {
        unsafe {
            let ptr = E::try_catch(self as *const CxxExceptionPtr as *const c_void);
            (ptr as *const E).as_ref()
        }
    }

    /// Triggers a panic.
    ///
    /// If the pointer contained a panic encoded as a C++ exception, it will be normally resumed.
    /// Otherwise, triggers a panic that encapsulates this exception pointer and allows to rethrow
    /// the exception later when it crosses the Rust -> C++ boundary.
    #[cold]
    pub fn rethrow(self) -> ! {
        self.maybe_resume_panic();

        // Technically, we don't resume a previous panic, but we want to skip triggering the panic hook.
        std::panic::resume_unwind(Box::new(CxxPanicWrapper(self)))
    }

    /// Inspects a panic payload and recovers an exception pointer from it.
    #[inline]
    pub fn try_from_panic(
        panic_payload: Box<dyn Any + Send + 'static>,
    ) -> Result<Self, Box<dyn Any + Send + 'static>> {
        panic_payload.downcast::<CxxPanicWrapper>().map(|b| b.0)
    }

    /// Encodes given panic payload as a C++ exception so that it can be passed to C++.
    ///
    /// Not all types of panic payloads can be passed to C++ - only String and &str are supported.
    /// Those types are thrown by `panic!` and `assert!`, so it should be fine.
    /// In case of other types, this function will abort the process.
    pub fn panic_to_exception(panic_payload: Box<dyn Any + Send + 'static>) -> CxxExceptionPtr {
        // Unfortunately, we only support passing &str and String
        let s = match panic_payload.downcast_ref::<String>() {
            Some(s) => s.as_str(),
            None => match panic_payload.downcast_ref::<&str>() {
                Some(s) => *s,
                None => {
                    // Sadly, I don't know how to print the name of the type.
                    eprintln!("Cannot translate panic payload to C++ exception: unsupported type (type_id: {:?}", panic_payload.type_id());
                    std::process::abort()
                }
            },
        };
        let mut string_data = Vec::with_capacity(s.len() + 1);
        string_data.extend_from_slice(s.as_bytes());
        let c_str = CString::new(string_data).unwrap_or_else(|err| {
            let nul = err.nul_position();
            let mut s = err.into_vec();
            s.truncate(nul);
            CString::new(s).unwrap()
        });
        let mut eptr = MaybeUninit::<Self>::uninit();
        unsafe {
            seastar_rs_exception_from_rust_panic(
                c_str.as_c_str().as_ptr(),
                &mut eptr as *mut _ as *mut c_void,
            );
            eptr.assume_init()
        }
    }

    /// If this exception pointer is holding a panic, rethrows it.
    pub fn maybe_resume_panic(&self) {
        unsafe {
            let ptr = seastar_rs_exception_recover_panic_from_exception(
                self as *const _ as *const c_void,
            );
            if !ptr.is_null() {
                let cstr = CStr::from_ptr(ptr).to_owned();
                let payload = Box::new(cstr.into_string().unwrap());
                std::panic::resume_unwind(payload);
            }
        };
    }
}

struct CxxPanicWrapper(CxxExceptionPtr);

impl Clone for CxxExceptionPtr {
    #[inline]
    fn clone(&self) -> Self {
        let mut eptr = MaybeUninit::<Self>::uninit();
        unsafe {
            seastar_rs_exception_ptr_clone(
                &self as *const _ as *const c_void,
                &mut eptr as *mut _ as *mut c_void,
            );
            eptr.assume_init()
        }
    }
}

impl Drop for CxxExceptionPtr {
    #[inline]
    fn drop(&mut self) {
        unsafe {
            seastar_rs_exception_ptr_drop(self as *mut _ as *mut c_void);
        }
    }
}

unsafe impl ::cxx::ExternType for CxxExceptionPtr {
    type Id = ::cxx::type_id!(std::exception_ptr);
    type Kind = ::cxx::kind::Trivial;
}

/// An exception that Rust can manipulate through CxxExceptionPtr.
pub unsafe trait CxxExceptionPtrTarget {
    #[doc(hidden)]
    unsafe fn try_catch(ptr_to_eptr: *const c_void) -> *const c_void;
}

#[doc(hidden)]
pub mod internal {
    use std::ffi::{c_void, CStr};

    pub unsafe fn write_exception_what(
        ptr_to_std_exception: *const c_void,
        f: &mut std::fmt::Formatter<'_>,
    ) -> std::fmt::Result {
        let what = CStr::from_ptr(super::seastar_rs_exception_get_what(ptr_to_std_exception));
        // TODO: `to_string_lossy` allocates if `what` is not a valid utf8 string.
        // After `Utf8Chunk` is stabilized, we will get a nice way to do this without allocations.
        // https://github.com/rust-lang/rust/issues/99543
        f.write_str(&what.to_string_lossy())
    }
}

extern "C" {
    fn seastar_rs_exception_ptr_clone(ptr_to_eptr: *const c_void, dst: *mut c_void);
    fn seastar_rs_exception_ptr_drop(ptr_to_eptr: *mut c_void);
    fn seastar_rs_exception_get_what(ptr_to_std_exception: *const c_void) -> *const c_char;
    fn seastar_rs_exception_from_rust_panic(c_str: *const c_char, dst: *mut c_void);
    fn seastar_rs_exception_recover_panic_from_exception(
        ptr_to_eptr: *const c_void,
    ) -> *const c_char;
}

seastar_macros::gen_py!("idl/exceptions_std.idl.yaml");
seastar_macros::gen_py!("idl/exceptions_seastar.idl.yaml");
