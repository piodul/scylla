use std::cell::UnsafeCell;
use std::future::Future;
use std::pin::Pin;
use std::rc::Rc;
use std::task::{Context, Poll, Waker};

pub fn oneshot<T>() -> (Sender<T>, Receiver<T>) {
    let shared1 = Rc::new(OneshotCell::new());
    let shared2 = Rc::clone(&shared1);
    (
        Sender {
            shared: Some(shared1),
        },
        Receiver { shared: shared2 },
    )
}

pub struct Sender<T> {
    shared: Option<Rc<OneshotCell<T>>>,
}

impl<T> Sender<T> {
    #[inline(always)]
    pub fn send(mut self, t: T) -> Result<(), T> {
        self.shared.take().unwrap().send(t)
    }
}

impl<T> Drop for Sender<T> {
    fn drop(&mut self) {
        if let Some(cell) = &self.shared {
            cell.close_sender();
        }
    }
}

pub struct Receiver<T> {
    shared: Rc<OneshotCell<T>>,
}

impl<T> Future for Receiver<T> {
    type Output = Result<T, Closed>;

    #[inline]
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        self.shared.poll_recv(cx)
    }
}

pub(crate) struct OneshotCell<T> {
    shared: UnsafeCell<Shared<T>>,
}

// Safety (with respect to accessing `shared`):
// - We only access `shared` from this type's methods
// - The type is not sync, so methods can only be called on one thread
// - Methods are not reentrant, so they cannot run concurrently
//   and no two mutable references are created
impl<T> OneshotCell<T> {
    #[inline]
    pub fn new() -> Self {
        Self {
            shared: UnsafeCell::new(Shared::Pending),
        }
    }

    #[inline]
    pub fn send(&self, t: T) -> Result<(), T> {
        let shared = unsafe { &mut *self.shared.get() };
        if matches!(shared, Shared::Closed) {
            return Err(t);
        }
        match std::mem::replace(shared, Shared::Ready(t)) {
            Shared::Pending => Ok(()),
            Shared::PendingWaited(w) => {
                w.wake();
                Ok(())
            }
            Shared::Ready(_) => panic!("OneshotCell polled after being marked as ready"),
            Shared::Closed => panic!("OneshotCell already closed"),
        }
    }

    #[inline]
    pub fn close_sender(&self) {
        let shared = unsafe { &mut *self.shared.get() };
        *shared = Shared::Closed;
    }

    #[inline]
    pub fn poll_recv(&self, cx: &mut Context<'_>) -> Poll<Result<T, Closed>> {
        let shared = unsafe { &mut *self.shared.get() };
        match shared {
            Shared::Pending => {
                *shared = Shared::PendingWaited(cx.waker().clone());
                Poll::Pending
            }
            Shared::PendingWaited(old_waker) => {
                if !old_waker.will_wake(cx.waker()) {
                    *old_waker = cx.waker().clone();
                }
                Poll::Pending
            }
            Shared::Ready(_) => match std::mem::replace(shared, Shared::Closed) {
                Shared::Ready(t) => Poll::Ready(Ok(t)),
                _ => unreachable!(),
            },
            Shared::Closed => Poll::Ready(Err(Closed)),
        }
    }
}

#[derive(Debug)]
pub struct Closed;

enum Shared<T> {
    /// The channel has not produced a value yet and hasn't been polled yet.
    /// TODO: We can get rid of this state and use noop_waker after it is stabilized
    Pending,

    /// The channel has not produced a value yet but has been polled.
    PendingWaited(Waker),

    /// The channel has produced a value and it needs to be consumed by the receiver.
    Ready(T),

    /// The channel has been closed from either side.
    Closed,
}
