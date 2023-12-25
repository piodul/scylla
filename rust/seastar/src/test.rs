use std::cell::Cell;
use std::rc::Rc;

use crate::abort_source::AbortSource;
use crate::exception::AbortRequestedException;
use crate::exception::CxxException;
use crate::exception::CxxExceptionPtr;
use crate::exception::ResultExt;
use crate::future::BoxFuture;
use crate::promise::BoxPromise;
use crate::sched;
use crate::task;

#[cxx::bridge]
mod ffi {
    #[namespace = "seastar::rs::generated"]
    extern "C++" {
        include!("rust/cxx/future.hh");
        include!("rust/cxx/promise.hh");
        include!("rust/seastar/idl/futures_promises_primitive.idl.hh");

        type BoxFutureUnit = crate::BoxFutureUnit;
        type BoxFutureBool = crate::BoxFutureBool;
        type BoxFutureU32 = crate::BoxFutureU32;

        type BoxPromiseBool = crate::BoxPromiseBool;
    }

    #[namespace = "std"]
    extern "C++" {
        include!("rust/cxx/exception.hh");

        #[cxx_name = "exception_ptr"]
        type CxxExceptionPtr = crate::exception::CxxExceptionPtr;
    }

    #[namespace = "seastar"]
    extern "C++" {
        include!(<seastar/core/abort_source.hh>);

        #[cxx_name = "abort_source"]
        type AbortSource = crate::AbortSource;
    }

    #[namespace = "seastar::rs::test"]
    extern "Rust" {
        fn test1(b: bool) -> BoxFutureBool;
        fn test2(p: BoxPromiseBool, v: bool);
        fn test3(b: bool) -> BoxFutureBool;
        fn test4(f: BoxFutureU32) -> BoxFutureU32;
        fn test5(eptr: CxxExceptionPtr) -> &'static str;

        fn instantiate_a_panic(message: &str) -> CxxExceptionPtr;
        fn consume_panic(eptr: CxxExceptionPtr) -> String;

        fn test_exception_repackaging(f: BoxFutureBool) -> BoxFutureBool;

        fn test_submit_to() -> BoxFutureUnit;

        fn test_abort_source(abs: UniquePtr<AbortSource>) -> BoxFutureUnit;
    }

    impl UniquePtr<AbortSource> {}
}

fn test1(b: bool) -> BoxFuture<bool> {
    crate::future::make_ready_future(b)
}

fn test2(p: BoxPromise<bool>, v: bool) {
    p.set_value(v);
}

fn test3(b: bool) -> BoxFuture<bool> {
    task::spawn_for_cpp(async move {
        sched::yield_now().await;
        b
    })
}

fn test4(f: BoxFuture<u32>) -> BoxFuture<u32> {
    task::spawn_for_cpp(async move { 2 * f.await.eunwrap() })
}

fn test5(eptr: CxxExceptionPtr) -> &'static str {
    // Expect an abort_requested_exception

    // Must not be null
    if eptr.is_null() {
        return "null";
    }

    // Must also match as CxxException
    if eptr.try_catch::<&CxxException>().is_none() {
        return "not-cxx-exception";
    }

    if let Some(e) = eptr.try_catch::<&AbortRequestedException>() {
        if e.to_string() != "abort requested" {
            return "wrong message";
        }
        return "ok";
    }
    "wrong type"
}

fn instantiate_a_panic(message: &str) -> CxxExceptionPtr {
    let p = std::panic::catch_unwind(|| panic!("{}", message)).unwrap_err();
    CxxExceptionPtr::panic_to_exception(p)
}

fn consume_panic(eptr: CxxExceptionPtr) -> String {
    match std::panic::catch_unwind(|| eptr.rethrow()) {
        Ok(_) => "<not a panic>".to_string(),
        Err(payload) => match payload.downcast::<String>() {
            Ok(s) => *s,
            Err(_) => "<not a string payload>".to_string(),
        },
    }
}

#[crate::taskify(crate = crate)]
async fn test_exception_repackaging(f: BoxFuture<bool>) -> bool {
    f.await.eunwrap()
}

#[crate::taskify(crate = crate)]
async fn test_submit_to() {
    assert!(crate::smp::shard_count() > 0);

    for shard_id in 0..crate::smp::shard_count() {
        let remote_shard_id =
            crate::task::submit_to(shard_id, || async move { crate::smp::this_shard() }).await;
        assert_eq!(shard_id, remote_shard_id);
    }
}

#[crate::taskify(crate = crate)]
async fn test_abort_source(abs: cxx::UniquePtr<AbortSource>) {
    assert!(!abs.abort_requested());
    assert!(abs.check().is_ok());

    let flag = Rc::new(Cell::new(0u32));

    let flag2 = Rc::clone(&flag);
    let sub = abs.subscribe(move |_| {
        flag2.set(flag2.get() + 1);
    });
    assert!(!sub.is_null());
    std::mem::drop(sub);

    let flag2 = Rc::clone(&flag);
    let sub = abs.subscribe(move |_| {
        flag2.set(flag2.get() + 2);
    });
    assert!(!sub.is_null());

    assert!(!abs.abort_requested());
    assert!(abs.check().is_ok());

    abs.request_abort();
    assert!(abs.abort_requested());
    assert!(abs.check().is_err());
    assert_eq!(flag.get(), 2);
}
