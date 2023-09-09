use std::ffi::{c_char, c_void};
use std::future::Future;
use std::marker::PhantomPinned;
use std::pin::Pin;
use std::task::{Context, Poll, Waker};

use self::ffi::AbortSourceSubscription;

#[cxx::bridge(namespace = "seastar")]
mod ffi {
    unsafe extern "C++" {
        include!("seastar/core/abort_source.hh");
        include!("rust/bridge/abort_source.hh");

        /// Facility to communicate a cancellation request.
        #[cxx_name = "abort_source"]
        type AbortSource = crate::abort_source::AbortSource;
        fn abort_requested(self: &AbortSource) -> bool;

        #[namespace = "ffi"]
        #[cxx_name = "abort_source_subscription"]
        type AbortSourceSubscription<'a>;

        #[namespace = "ffi"]
        fn create_subscription<'a>(
            source: &'a AbortSource,
        ) -> UniquePtr<AbortSourceSubscription<'a>>;
        fn abort_requested(self: &AbortSourceSubscription) -> bool;
        unsafe fn link_with_waker(
            self: Pin<&mut AbortSourceSubscription>,
            waker_opt_ptr: *mut c_char,
        );
    }

    impl UniquePtr<AbortSource> {}
}

#[repr(C)]
pub struct AbortSource {
    dummy: (),
}

unsafe impl ::cxx::ExternType for AbortSource {
    type Id = ::cxx::type_id!("seastar::abort_source");
    type Kind = ::cxx::kind::Opaque;
}

impl AbortSource {
    // Not using auto-generated bindings, because it would impose &mut self
    // on this method (it's not const in the C++ definition) and would make
    // it pretty useless.
    pub fn request_abort(&self) -> bool {
        extern "C" {
            #[link_name = "seastar_rs_cpp_abort_source_request_abort"]
            fn impl_fn(this: *const c_void) -> bool;
        }
        unsafe { impl_fn(self as *const AbortSource as *const c_void) }
    }
}

pub fn with_abort_source<F, T>(source: &AbortSource, fut: F) -> AbortableFuture<F>
where
    F: Future<Output = T>,
{
    AbortableFuture {
        inner: fut,
        subscription: ffi::create_subscription(source),
        waker: None,
        _pinned: PhantomPinned,
    }
}

pub struct AbortableFuture<'a, F> {
    inner: F,
    subscription: cxx::UniquePtr<AbortSourceSubscription<'a>>,
    waker: Option<Waker>,
    _pinned: PhantomPinned,
}

impl<'a, F, T> Future for AbortableFuture<'a, F>
where
    F: Future<Output = T>,
{
    type Output = Result<T, AbortedError>;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        // We like to live dangerously, for now
        let me = unsafe { self.get_unchecked_mut() };

        // Are we aborted yet?
        if me.subscription.abort_requested() {
            return Poll::Ready(Err(AbortedError));
        }

        // Not aborted yet. Try to poll.
        let inner = unsafe { Pin::new_unchecked(&mut me.inner) };
        if let Poll::Ready(v) = inner.poll(cx) {
            return Poll::Ready(Ok(v));
        }

        // The inner future was polled but is not ready yet.
        // We need to store the waker ourselves and make the subscription
        // point to it so that it can wake us.
        me.waker = Some(cx.waker().clone());
        unsafe {
            me.subscription
                .as_mut()
                .unwrap()
                .link_with_waker(&mut me.waker as *mut Option<Waker> as *mut c_char);
        }
        Poll::Pending
    }
}

pub struct AbortedError;
