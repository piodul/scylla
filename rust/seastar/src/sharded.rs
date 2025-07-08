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
use futures_util::{stream, StreamExt};

use crate::foreign::ForeignCell;

pub type LocalBoxFuture<'a, T> = Pin<Box<dyn Future<Output = T> + 'a>>;

// Phew, complicated type
type ShardInstanceSlice<S> = [ForeignCell<Cell<Option<ShardInstanceAndHandle<S>>>>];

struct ShardInstanceAndHandle<S: RustShardable> {
    // Arc wrapped in Rc - to avoid atomics when cloning
    handle: Rc<ShardedHandle<S>>,
    instance: Rc<S>,
}

/// A Rust service that can be put into Sharded<T>.
// TODO: Convert to use async after it is stabilized in traits
pub trait RustShardable: Sized + 'static {
    type ConstructArgs: Sync;

    /// Creates a new shard-local instance.
    ///
    /// The `handle` can be cloned and stored in the shard-local instance. However, until [`Sharded::new`] completes
    /// it is not guaranteed that instances of other shards are constructed, so the handle must not be used to access
    /// other shards. Trying to access other shards' instances might result in a panic.
    ///
    /// TODO: Should this return a Result?
    fn construct(
        handle: &ShardedHandle<Self>,
        args: &Self::ConstructArgs,
    ) -> LocalBoxFuture<'static, Self>;

    /// Called when the service is about to stop.
    ///
    /// This method will be called in parallel on all shards. Only after it completes on all shards
    /// the shard-local instances will be destroyed.
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
        // Phase 1: Invoke stop() on all instances
        self.invoke_on_all(|h| {
            let me = h.local();
            async move { me.stop().await }
        })
        .await;

        // Phase 2: Drop the instances
        join_all((0..crate::smp::shard_count()).map(|shard| {
            let handle = ForeignCell::new(ShardedImpl(&self.handle.instances).local_handle());
            crate::task::submit_to(shard, move || async move {
                ShardedImpl(&handle.get_deref().instances)
                    .local_ref()
                    .take()
                    .unwrap();

                // TODO: Wait until all references are freed?
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

    pub fn invoke_on_all<Func, Fut>(&self, f: Func) -> impl Future<Output = ()>
    where
        Func: FnOnce(&ShardedHandle<S>) -> Fut + Send + Clone + 'static,
        Fut: Future<Output = ()> + 'static,
    {
        self.handle.invoke_on_all(f)
    }

    pub fn try_invoke_on_all<E, Func, Fut>(&self, f: Func) -> impl Future<Output = Result<(), E>>
    where
        E: Send + 'static,
        Func: FnOnce(&ShardedHandle<S>) -> Fut + Send + Clone + 'static,
        Fut: Future<Output = Result<(), E>> + 'static,
    {
        self.handle.try_invoke_on_all(f)
    }

    pub fn map_collect<C, T, Func, Fut>(&self, f: Func) -> impl Future<Output = C>
    where
        C: Default + Extend<T>,
        T: Send + 'static,
        Func: FnOnce(&ShardedHandle<S>) -> Fut + Send + Clone + 'static,
        Fut: Future<Output = T> + 'static,
    {
        self.handle.map_collect(f)
    }

    pub fn try_map_collect<C, T, E, Func, Fut>(&self, f: Func) -> impl Future<Output = Result<C, E>>
    where
        C: Default + Extend<T>,
        T: Send + 'static,
        E: Send + 'static,
        Func: FnOnce(&ShardedHandle<S>) -> Fut + Send + Clone + 'static,
        Fut: Future<Output = Result<T, E>> + 'static,
    {
        self.handle.try_map_collect(f)
    }
}

/// A non-owning handle to a [`Sharded<S>`](Sharded) instance.
///
/// The purpose of this type is to make it possible for local instances of `S` to refer
/// to other instances from the same `Sharded`. The handle is passed during [`RustShardable::construct`]
/// and can be stored by the shard-local instance so that later it can send tasks to other shards.
pub struct ShardedHandle<S: RustShardable> {
    instances: Arc<ShardInstanceSlice<S>>,
}

impl<S: RustShardable> ShardedHandle<S> {
    pub fn invoke_on<T, Func, Fut>(&self, shard: usize, f: Func) -> impl Future<Output = T>
    where
        T: Send + 'static,
        Func: FnOnce(&ShardedHandle<S>) -> Fut + Send + 'static,
        Fut: Future<Output = T> + 'static,
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

    pub fn invoke_on_all<Func, Fut>(&self, f: Func) -> impl Future<Output = ()>
    where
        Func: FnOnce(&ShardedHandle<S>) -> Fut + Send + Clone + 'static,
        Fut: Future<Output = ()> + 'static,
    {
        self.map_collect(f)
    }

    pub fn try_invoke_on_all<E, Func, Fut>(&self, f: Func) -> impl Future<Output = Result<(), E>>
    where
        E: Send + 'static,
        Func: FnOnce(&ShardedHandle<S>) -> Fut + Send + Clone + 'static,
        Fut: Future<Output = Result<(), E>> + 'static,
    {
        self.try_map_collect(f)
    }

    pub fn map_collect<C, T, Func, Fut>(&self, f: Func) -> impl Future<Output = C>
    where
        C: Default + Extend<T>,
        T: Send + 'static,
        Func: FnOnce(&ShardedHandle<S>) -> Fut + Send + Clone + 'static,
        Fut: Future<Output = T> + 'static,
    {
        // Calling `self.invoke_on` spawns tasks. Polling the returned futures
        // is not needed to drive the tasks to completion.
        let mut futs = Vec::with_capacity(self.instances.len());
        for shard in 0..(self.instances.len() - 1) {
            futs.push(self.invoke_on(shard, f.clone()));
        }
        // Avoid clone when pushing the last one
        futs.push(self.invoke_on(self.instances.len(), f));

        async move {
            let mut ret = C::default();
            // ret.extend_reserve(futs.len()); // TODO: Uncomment after the method is stabilized
            for f in futs {
                ret.extend(std::iter::once(f.await));
            }
            ret
        }
    }

    pub fn try_map_collect<C, T, E, Func, Fut>(&self, f: Func) -> impl Future<Output = Result<C, E>>
    where
        C: Default + Extend<T>,
        T: Send + 'static,
        E: Send + 'static,
        Func: FnOnce(&ShardedHandle<S>) -> Fut + Send + Clone + 'static,
        Fut: Future<Output = Result<T, E>> + 'static,
    {
        struct ResultExtender<T, E> {
            state: Result<T, E>,
        }
        impl<T, E> Default for ResultExtender<T, E>
        where
            T: Default,
        {
            fn default() -> Self {
                Self {
                    state: Ok(T::default()),
                }
            }
        }
        impl<T, E, A> Extend<Result<A, E>> for ResultExtender<T, E>
        where
            T: Extend<A>,
        {
            fn extend<U: IntoIterator<Item = Result<A, E>>>(&mut self, iter: U) {
                let mut iter = iter.into_iter();
                while let Ok(t) = &mut self.state {
                    match iter.next() {
                        Some(Ok(u)) => {
                            t.extend(std::iter::once(u));
                        }
                        Some(Err(e)) => {
                            self.state = Err(e);
                            return;
                        }
                        None => return,
                    }
                }
            }

            // TODO: implement extend_one after it gets stabilized
        }

        let fut = self.map_collect::<ResultExtender<C, E>, _, _, _>(f);
        async move { fut.await.state }
    }

    #[inline]
    pub fn local(&self) -> Rc<S> {
        ShardedImpl(&*self.instances).local()
    }
}

impl<S: RustShardable> Clone for ShardedHandle<S> {
    #[inline]
    fn clone(&self) -> Self {
        Self {
            instances: self.instances.clone(),
        }
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
            .expect("local instance of sharded struct not initialized");
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
            .expect("local instance of sharded struct not initialized");
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
