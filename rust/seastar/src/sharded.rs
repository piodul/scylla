/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

use std::cell::Cell;
use std::future::Future;
use std::pin::Pin;
use std::rc::Rc;
use std::sync::Arc;

use futures_util::future::join_all;

use crate::foreign::ForeignCell;

use futures_util::{stream, StreamExt};

type LocalBoxFuture<'a, T> = Pin<Box<dyn Future<Output = T> + 'a>>;

// Phew, complicated type
type ShardInstanceSlice<S> = [ForeignCell<Cell<Option<ShardInstanceAndHandle<S>>>>];

struct ShardInstanceAndHandle<S: RustShardable> {
    // Arc wrapped in Rc - to avoid atomics when cloning
    handle: Rc<ShardedHandle<S>>,
    instance: Rc<S>,
}

/// A Rust service that can be put into Sharded<T>.
pub trait RustShardable: Sized + 'static {
    type ConstructArgs: Sync;

    /// Creates a new shard-local instance.
    /// TODO: Should this return a Result?
    fn construct(
        handle: &ShardedHandle<Self>,
        args: &Self::ConstructArgs,
    ) -> LocalBoxFuture<'static, Self>;

    /// Called when the service is about to stop.
    fn stop(&self) -> LocalBoxFuture<()> {
        Box::pin(async {})
    }
}

pub struct Sharded<S: RustShardable> {
    handle: ShardedHandle<S>,
}

impl<S: RustShardable> Sharded<S> {
    /// Creates a new, initialized Sharded<S> object.
    pub async fn new(args: S::ConstructArgs) -> Self {
        // Phase 1: Create a vector of instances, for now empty.
        // We must communicate with other shards to do so in order to
        // construct proper ForeignCells, even though we only construct Nones.

        let instances = join_all((0..crate::smp::shard_count()).map(|shard| {
            crate::task::submit_to(shard, move || async { ForeignCell::new(Cell::new(None)) })
        }))
        .await;

        // Phase 2: Construct the instances, passing a SharedHandle and arguments
        // to each one of them
        let instances: Arc<_> = instances.into_boxed_slice().into();
        let args = Arc::new(ForeignCell::new(args));

        stream::iter(0..crate::smp::shard_count())
            .for_each_concurrent(None, |shard| {
                let instances = Arc::clone(&instances);
                let instances2 = Arc::clone(&instances);
                let args = Arc::clone(&args);
                crate::task::submit_to(shard, move || async move {
                    let args = ForeignCell::get(&args);
                    let handle = Rc::new(ShardedHandle { instances });
                    let instance = S::construct(&handle, &args).await;
                    let instance = Rc::new(instance);
                    let siac = ShardInstanceAndHandle { handle, instance };
                    ShardedImpl(&*instances2).local_ref().set(Some(siac));

                    // TODO: Pass args back to the original shard in order to do the atomic decrement there
                    // and reduce passing the cache line between shards
                })
            })
            .await;

        let handle = ShardedHandle { instances };
        Self { handle }
    }

    pub async fn stop(self) {
        join_all((0..crate::smp::shard_count()).map(|shard| {
            let handle = ForeignCell::new(ShardedImpl(&self.handle.instances).local_handle());
            crate::task::submit_to(shard, move || async move {
                let s = ShardedImpl(&handle.get_deref().instances)
                    .local_ref()
                    .take()
                    .unwrap();
                s.instance.stop().await;
                std::mem::drop(s);

                // TODO: Wait until all references are freed
            })
        }))
        .await;
    }

    #[inline]
    pub fn local(&self) -> Rc<S> {
        self.handle.local()
    }

    pub fn invoke_on<Fun, Fut, T>(&self, shard: usize, f: Fun) -> impl Future<Output = T>
    where
        Fun: FnOnce(&ShardedHandle<S>) -> Fut + Send + 'static,
        Fut: Future<Output = T> + 'static,
        T: Send + 'static,
    {
        self.handle.invoke_on(shard, f)
    }
}

/// A non-owning handle to a Sharded<S> instance.
///
/// This type is intended as an aid for
///
/// Not cloneable in order not to encourage atomic operations.
pub struct ShardedHandle<S: RustShardable> {
    instances: Arc<ShardInstanceSlice<S>>,
}

impl<S: RustShardable> ShardedHandle<S> {
    pub fn invoke_on<Func, Fut, T>(&self, shard: usize, f: Func) -> impl Future<Output = T>
    where
        Func: FnOnce(&ShardedHandle<S>) -> Fut + Send + 'static,
        Fut: Future<Output = T> + 'static,
        T: Send + 'static,
    {
        let handle = ForeignCell::new(ShardedImpl(&self.instances).local_handle());
        let fut = crate::task::submit_to(shard, move || async move {
            let ret = f(handle.get_deref()).await;

            // Pass the handle back to the shard where it was constructed
            // in order to avoid additional submit_to from Foreign's constructor
            (handle, ret)
        });
        async move {
            let (_handle, ret) = fut.await;
            ret
        }
    }

    #[inline]
    pub fn local(&self) -> Rc<S> {
        ShardedImpl(&*self.instances).local()
    }
}

struct ShardedImpl<'a, S: RustShardable>(&'a ShardInstanceSlice<S>);

impl<'a, S: RustShardable> ShardedImpl<'a, S> {
    #[inline]
    fn local(&self) -> Rc<S> {
        // `Cell` does not allow cloning the contents.
        // Replace the contents with None, clone, put it back, return the cloned value
        // Hopefully, the compiler will be able to optimize it out
        let c = self.local_ref();
        let siac = c
            .take()
            .expect("local instance of shared struct not initialized");
        let s = Rc::clone(&siac.instance);
        c.set(Some(siac));
        s
    }

    #[inline]
    fn local_handle(&self) -> Rc<ShardedHandle<S>> {
        // `Cell` does not allow cloning the contents.
        // Replace the contents with None, clone, put it back, return the cloned value
        // Hopefully, the compiler will be able to optimize it out
        let c = self.local_ref();
        let siac = c
            .take()
            .expect("local instance of shared struct not initialized");
        let h = Rc::clone(&siac.handle);
        c.set(Some(siac));
        h
    }

    #[inline]
    fn local_ref(&self) -> &Cell<Option<ShardInstanceAndHandle<S>>> {
        match self.0[crate::smp::this_shard()].try_get() {
            // i-th instance belongs to i-th shard, so this should not happen
            Err(_) => unreachable!(),
            Ok(c) => c,
        }
    }
}
