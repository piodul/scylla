/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

use std::ffi::{c_int, c_void};
use std::mem::MaybeUninit;

use crate::exception::{CxxExceptionPtr, Result};

// This represents an original, external abort_source
pub struct AbortSource {
    // Opaque type
    _data: [MaybeUninit<*const c_void>; 0],
}

unsafe impl ::cxx::ExternType for AbortSource {
    type Id = ::cxx::type_id!(seastar::abort_source);
    type Kind = ::cxx::kind::Opaque;
}

impl AbortSource {
    #[inline]
    pub fn subscribe<F>(&self, f: F) -> cxx::UniquePtr<AbortSourceSubscription>
    where
        F: FnOnce(Option<CxxExceptionPtr>) + 'static,
    {
        let caller = subscribe_fn_caller::<F>;
        let dropper = subscribe_fn_dropper::<F>;
        let payload = Box::into_raw(Box::new(f)) as *mut c_void;
        let mut out = MaybeUninit::uninit();
        unsafe {
            seastar_rs_abort_source_subscribe(
                self as *const _ as *const c_void,
                payload,
                caller,
                dropper,
                out.as_mut_ptr() as *mut c_void,
            );
            out.assume_init()
        }
    }
    #[inline]
    pub fn request_abort(&self) {
        unsafe {
            seastar_rs_abort_source_request_abort(self as *const _ as *const c_void);
        }
    }

    #[inline]
    pub fn request_abort_with_exception(&self, eptr: CxxExceptionPtr) {
        let mut eptr = MaybeUninit::new(eptr); // Consumed by C++
        unsafe {
            seastar_rs_abort_source_request_abort_with_exception(
                self as *const _ as *const c_void,
                eptr.as_mut_ptr() as *mut c_void,
            );
        }
    }
    #[inline]
    pub fn abort_requested(&self) -> bool {
        unsafe { seastar_rs_abort_source_abort_requested(self as *const _ as *const c_void) != 0 }
    }

    #[inline]
    pub fn check(&self) -> Result<()> {
        let mut eptr = MaybeUninit::uninit();
        unsafe {
            let has_eptr = seastar_rs_abort_source_check(
                self as *const _ as *const c_void,
                eptr.as_mut_ptr() as *mut c_void,
            );
            if has_eptr == 0 {
                Ok(())
            } else {
                Err(eptr.assume_init())
            }
        }
    }
}

extern "C" {
    fn seastar_rs_abort_source_subscribe(
        abs: *const c_void,
        payload: *mut c_void,
        caller: unsafe extern "C" fn(f: *mut c_void, eptr: *const c_void),
        dropper: unsafe extern "C" fn(f: *mut c_void),
        out_unique_ptr: *mut c_void,
    );

    fn seastar_rs_abort_source_request_abort(abs: *const c_void);
    fn seastar_rs_abort_source_request_abort_with_exception(abs: *const c_void, in_eptr: *mut c_void);
    fn seastar_rs_abort_source_abort_requested(abs: *const c_void) -> c_int;
    fn seastar_rs_abort_source_check(abs: *const c_void, out_eptr: *mut c_void) -> c_int;
}

unsafe extern "C" fn subscribe_fn_caller<F>(f: *mut c_void, abs: *const c_void)
where
    F: FnOnce(Option<CxxExceptionPtr>) + 'static,
{
    let abs = abs as *const CxxExceptionPtr;
    let abs = (!abs.is_null()).then(|| CxxExceptionPtr::clone(&*abs));
    let f = Box::from_raw(f as *mut F);
    (*f)(abs);
}

// TODO: This could be in utils
unsafe extern "C" fn subscribe_fn_dropper<F>(f: *mut c_void)
where
    F: 'static,
{
    let _ = Box::from_raw(f as *mut F);
}

pub struct AbortSourceSubscription {
    // Opaque type
    _data: [MaybeUninit<*const c_void>; 0],
}

unsafe impl ::cxx::ExternType for AbortSourceSubscription {
    type Id = ::cxx::type_id!(seastar::abort_source::subscription);
    type Kind = ::cxx::kind::Opaque;
}

// Generate cxx glue code to allow passing abort_source::subscription through std::unique_ptr
#[cxx::bridge]
mod ffi {
    #[namespace = "seastar::abort_source"] // It's not really a namespace, but cxx needs full path
    extern "C++" {
        include!("seastar/core/abort_source.hh");

        #[cxx_name = "subscription"]
        type AbortSourceSubscription = crate::abort_source::AbortSourceSubscription;
    }
    impl UniquePtr<AbortSourceSubscription> {}
}
