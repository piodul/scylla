/*
 * Copyright (C) 2022-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

use std::cell::{Cell, RefCell};
use std::collections::hash_map::Entry;
use std::collections::{HashMap, HashSet};
use std::rc::Rc;

use ffi::Listener;
use seastar::exception::ResultExt;
use seastar::native::abort_source::AbortSource;
use seastar::native::condition_variable::ConditionVariable;
use seastar::sharded::{LocalBoxFuture, RustShardable, ShardedHandle};
use seastar::smp;
use utils::Uuid;

#[cxx::bridge]
mod ffi {
    #[namespace = "seastar::rs::generated"]
    extern "C++" {
        type BoxFutureUnit = seastar::BoxFutureUnit;
        type BoxFutureBool = seastar::BoxFutureBool;
    }

    #[namespace = "utils"]
    extern "C++" {
        type Uuid = utils::Uuid;
    }

    #[namespace = "direct_failure_detector"]
    unsafe extern "C++" {
        include!("direct_failure_detector/failure_detector.hh");

        #[cxx_name = "pinger"]
        type Pinger;
        fn ping(self: &Pinger) -> BoxFutureBool;

        #[cxx_name = "clock"]
        type Clock;
        fn now(self: &Clock) -> i64;
        fn sleep_until(self: &Clock, tp: i64) -> BoxFutureUnit; // TODO: AbortSource

        #[cxx_name = "listener"]
        type Listener;
        fn mark_alive(self: &Listener, endpoint_id: Uuid) -> BoxFutureUnit;
        fn mark_dead(self: &Listener, endpoint_id: Uuid) -> BoxFutureUnit;
    }

    #[namespace = "direct_failure_detector"]
    extern "Rust" {
        #[cxx_name = "subscription"]
        type Subscription;
    }

    // Direct failure detector will have to be exposed via some codegen, with Sharded<>?
}

unsafe impl Send for Listener {}
unsafe impl Sync for Listener {}

type ClockInterval = i64;
type EndpointId = Uuid;

struct Subscription {
    fd_handle: ShardedHandle<FailureDetector>,
    listener_id: ListenerId,
    listener_ptr: *const ffi::Listener,
}

// Information about a listener registered on a given shard.
#[derive(Hash, PartialEq, Eq, Copy, Clone)]
struct ListenerId {
    // Shard-local index of the listener.
    id: usize,

    // Number of the shard the relevant listener originates from.
    shard: usize,
}

// Tracks the liveness of a given endpoint for a given listener threshold.
// See `endpoint_worker::ping_fiber()` and `endpoint_worker::notify_fiber()`.
#[derive(Default)]
struct EndpointLiveness {
    alive: bool,
    marked_alive: bool,
}

#[derive(Default)]
struct ListenersLiveness {
    // Vector of all listeners with the same threshold.
    listeners: Vec<ListenerId>,

    // For each endpoint managed by this shard, the liveness state of this endpoint shared by all listeners in `listeners`.
    endpoint_liveness: HashMap<EndpointId, EndpointLiveness>,
}

#[derive(Copy, Clone, PartialEq, Eq)]
enum EndpointUpdate {
    Added,
    Removed,
}

struct EndpointWorker {
    // Used when this worker is destroyed, either because the endpoint is removed from detected set
    // or the failure detector service is stopped.
    abs: AbortSource,

    // When `ping_fiber()` changes the liveness state of an endpoint (`endpoint_liveness::alive`), it signals
    // this condition variable. `notify_fiber()` sleeps on it; on wake up sends a notification and marks
    // that it sent the update (`endpoint_liveness:marked_alive`)
    alive_changed: ConditionVariable,

    // TODO: Think about those types
    ping_fiber: Cell<LocalBoxFuture<'static, ()>>,
    notify_fiber: Cell<LocalBoxFuture<'static, ()>>,
}

impl EndpointWorker {
    fn new() -> Rc<Self> {
        Rc::new(Self {
            abs: AbortSource::new(),
            alive_changed: ConditionVariable::new(),
            ping_fiber: Cell::new(Box::pin(async {})),
            notify_fiber: Cell::new(Box::pin(async {})),
        })
    }

    async fn ping_fiber(&self, fd: &FailureDetector, endpoint_id: EndpointId) {
        let pinger = fd.pinger;
        let clock = fd.clock;

        // `last_response` does not contain a valid value until we get the very first response to `ping()`.
        // That's fine since we don't use it until then (every use is protected with checking that at least one listener is `alive`,
        // which can only be true if there was a successful ping response).
        // Rust isn't smart enough to let us leave the variable uninitialized at the beginning,
        // so set it to 0 initially.
        let mut last_response = 0;

        while !self.abs.is_aborted() {
            let mut success = false;
            let start = clock.now();
            let mut next_ping_start = start + fd.ping_period;

            // A ping should take significantly less time than ping_period, but we give it a multiple of ping_period before it times out
            // just in case of transient network partitions.
            // However, if there's a listener that's going to timeout soon (before the ping returns), we abort the ping in order to handle
            // the listener (mark it as dead).
            let timeout = start
                + fd.listeners_liveness
                    .borrow()
                    .iter()
                    .filter(|(_, l)| l.endpoint_liveness[&endpoint_id].alive)
                    .map(|(threshold, _)| *threshold)
                    .chain(std::iter::once(3 * fd.ping_period))
                    .min()
                    .expect("empty iterator - impossible");

            if timeout > start {
                // TODO: ping_with_timeout
                match pinger.ping().await {
                    Ok(res) => success = res,
                    Err(err) => {
                        // TODO: Print it
                    }
                }
            } else {
                // We have a listener which already timed out.
                // Abandon the ping, instead proceed to marking it dead below and do the ping in the next iteration.
                next_ping_start = start;
            }

            let mut alive_changed = false;
            if success {
                last_response = clock.now();

                for ll in fd.listeners_liveness.borrow_mut().values_mut() {
                    let alive = &mut ll.endpoint_liveness.get_mut(&endpoint_id).unwrap().alive;
                    if !*alive {
                        *alive = true;
                        alive_changed = true;
                    }
                }
            } else {
                // Handle listeners which time-out before the next ping starts.
                // We could sleep until their threshold is actually crossed, but since we already know they will time-out
                // and there's no way to save them, it's simpler to just send the notifications immediately.
                for (threshold, ll) in fd.listeners_liveness.borrow_mut().iter_mut() {
                    let alive = &mut ll.endpoint_liveness.get_mut(&endpoint_id).unwrap().alive;
                    if *alive && last_response + *threshold <= next_ping_start {
                        *alive = false;
                        alive_changed = true;
                    }
                }
            }

            if alive_changed {
                self.alive_changed.signal();
            }

            clock.sleep_until(next_ping_start).await;
        }
    }

    // TODO: use a weak pointer for fd?
    async fn notify_fiber(&self, fd: &FailureDetector, endpoint_id: EndpointId) {
        let all_listeners_dead = || {
            fd.listeners_liveness
                .borrow()
                .values()
                .all(|ll| !ll.endpoint_liveness[&endpoint_id].alive)
        };

        let any_changed_liveness = || {
            fd.listeners_liveness.borrow().values().any(|ll| {
                let el = &ll.endpoint_liveness[&endpoint_id];
                el.alive != el.marked_alive
            })
        };

        loop {
            self.alive_changed
                .wait_until(|| {
                    (self.abs.is_aborted() && all_listeners_dead()) || any_changed_liveness()
                })
                .await;
            loop {
                // Introduce a scope so that `listeners_liveness` reference is destroyed
                // before an .await point.
                let futs = {
                    let mut listeners_liveness = fd.listeners_liveness.borrow_mut();
                    let Some(ll) = listeners_liveness.values_mut().find(|ll| {
                        let el = &ll.endpoint_liveness[&endpoint_id];
                        el.alive != el.marked_alive
                    }) else {
                        break;
                    };
                    let endpoint_liveness = ll.endpoint_liveness.get_mut(&endpoint_id).unwrap();
                    let alive = endpoint_liveness.alive;
                    assert_ne!(alive, endpoint_liveness.marked_alive);

                    // Spawn a bunch of tasks to tell listeners that the endpoint is alive or not.
                    // Note that we don't risk a panic from a RefCell because even if we schedule
                    // a task on this shard, it will only run after the next .await point - which
                    // happens out of scope of this block and after `listeners_liveness` reference
                    // is destroyed and does not dynamically borrow anymore.
                    ll.listeners
                        .iter()
                        .map(|li| {
                            let li = *li;
                            fd.handle.invoke_on(li.shard, move |h| {
                                let me = h.local();
                                async move { me.mark(li, endpoint_id, alive).await }
                            })
                        })
                        .collect::<Vec<_>>()
                };

                // Wait for the tasks.
                for fut in futs {
                    fut.await;
                }
            }

            // We check for shutdown at the end of the loop so we send final mark_dead notifications
            // before destroying the worker (see `failure_detector::impl::destroy_worker`).
            if self.abs.is_aborted() && all_listeners_dead() {
                return;
            }
        }
    }
}

struct FailureDetector {
    handle: Rc<ShardedHandle<Self>>,

    pinger: &'static ffi::Pinger,
    clock: &'static ffi::Clock,

    ping_period: ClockInterval,

    next_listener_id: Cell<usize>,
    local_listeners: RefCell<HashMap<ListenerId, &'static ffi::Listener>>,

    // Number of workers on each shard.
    // We use this to decide where to create new workers (we pick a shard with the smallest number of workers).
    // Used on shard 0 only.
    // The size of this vector is smp::count on shard 0 and it's empty on other shards.
    num_workers: Vec<Cell<usize>>,

    // For each endpoint in the detected set, the shard of its worker.
    // Used on shard 0 only.
    workers: RefCell<HashMap<EndpointId, usize>>,

    // The {add/remove}_endpoint user API only inserts the request into `_endpoint_updates` and signals `_endpoint_changed`.
    // The actual add/remove operation (which requires cross-shard ops) is performed by update_endpoint_fiber(),
    // which waits on the condition variable and removes elements from this map.
    // Used on shard 0 only.
    endpoint_updates: RefCell<HashMap<EndpointId, EndpointUpdate>>,
    endpoint_changed: ConditionVariable, // TODO: Change to condvar

    // Workers running on this shard.
    shard_workers: RefCell<HashMap<EndpointId, Rc<EndpointWorker>>>,

    // For each threshold:
    // - the set of all listeners registered with this threshold (this is replicated to every shard),
    // - the liveness state of all endpoints managed by this shard for this threshold.
    //
    // Each `endpoint_worker` running on this shard is managing, for each threshold, the `endpoint_liveness` state
    // at `listeners_liveness::endpoint_liveness[ep]`, where `ep` is the endpoint of that worker.
    listeners_liveness: RefCell<HashMap<ClockInterval, ListenersLiveness>>,

    // The listeners registered on this shard.
    registered: RefCell<HashSet<*const ffi::Listener>>,
}

/// Public interface
impl FailureDetector {
    // #[seastar::taskify]
    pub async fn register_listener(
        &self,
        listener: &'static ffi::Listener,
        threshold: ClockInterval,
    ) -> Subscription {
        if !self.registered.borrow_mut().insert(listener) {
            panic!(
                "direct_failure_detector: trying to register the same listener ({:p}) twice",
                listener as *const _,
            );
        }

        let id = ListenerId {
            id: self.next_listener_id.get(),
            shard: smp::this_shard(),
        };
        self.next_listener_id.set(id.id + 1);

        self.local_listeners.borrow_mut().insert(id, listener);

        self.handle
            .invoke_on_all(move |h| {
                h.local()
                    .listeners_liveness
                    .borrow_mut()
                    .entry(threshold)
                    .or_insert_with(Default::default)
                    .listeners
                    .push(id);
                std::future::ready(())
            })
            .await;

        Subscription {
            fd_handle: (*self.handle).clone(),
            listener_id: id,
        }
    }

    #[seastar::taskify]
    pub async fn add_endpoint(&self, endpoint_id: Uuid) {
        todo!()
    }

    #[seastar::taskify]
    pub async fn remove_endpoint(&self, endpoint_id: Uuid) {
        todo!()
    }
}

impl Drop for Subscription {
    fn drop(&mut self) {
        // Start by removing the listener from registered set which prevents the failure detector from dereferencing the listener.
        let fd = self.fd_handle.local();
        fd.registered.borrow().remove()
    }
}

// Private interface
impl FailureDetector {
    fn new_local(handle: Rc<ShardedHandle<Self>>) -> Self {
        Self {
            handle,
            pinger: todo!(),
            clock: todo!(),
            ping_period: Default::default(),
            next_listener_id: Default::default(),
            local_listeners: Default::default(),
            num_workers: Default::default(),
            workers: Default::default(),
            endpoint_updates: Default::default(),
            endpoint_changed: Default::default(),
            shard_workers: Default::default(),
            listeners_liveness: Default::default(),
            registered: Default::default(),
        }
    }

    fn send_update_endpoint(&self, endpoint_id: EndpointId, update: EndpointUpdate) {
        assert_eq!(smp::this_shard(), 0);
        self.endpoint_updates
            .borrow_mut()
            .insert(endpoint_id, update);

        self.endpoint_changed.signal();
    }

    async fn update_endpoint_fiber(&self) {
        assert_eq!(smp::this_shard(), 0);

        loop {
            self.endpoint_changed
                .wait_until(|| !self.endpoint_updates.borrow().is_empty())
                .await;

            // Fetch an update
            let (ep, update) = {
                let endpoint_updates = self.endpoint_updates.borrow();
                let ep = *endpoint_updates
                    .keys()
                    .next()
                    .expect("endpoint_updates should not be empty");
                let update = *endpoint_updates.get(&ep).unwrap();
                (ep, update)
            };

            // TOO: Error handling
            match update {
                EndpointUpdate::Added => self.do_add_endpoint(ep).await,
                EndpointUpdate::Removed => self.do_remove_endpoint(ep).await,
            }

            match self.endpoint_updates.borrow_mut().entry(ep) {
                Entry::Occupied(o) => {
                    if *o.get() == update {
                        // Safe to remove the entry.
                        o.remove();
                    } else {
                        // While we were updating the endpoint, the user requested the opposite update.
                        // Need to handle this endpoint again.
                    }
                }
                Entry::Vacant(_) => {
                    panic!("the entry was unexpectedly removed by somebody else")
                }
            }
        }
    }

    async fn do_add_endpoint(&self, endpoint_id: Uuid) {
        assert_eq!(smp::this_shard(), 0);

        if self.workers.borrow().contains_key(&endpoint_id) {
            return;
        }

        // Pick a shard with the smallest number of workers to create a new worker.
        let (shard, count) = self
            .num_workers
            .iter()
            .enumerate()
            .min_by_key(|(_, c)| c.get())
            .expect("`num_worker` variable is not supposed to be empty");

        count.set(count.get() + 1);
        self.workers.borrow_mut().insert(endpoint_id, shard);

        self.handle
            .invoke_on(shard, move |h| {
                h.local().create_worker(endpoint_id);
                std::future::ready(())
            })
            .await;
    }

    async fn do_remove_endpoint(&self, endpoint_id: Uuid) {
        assert_eq!(smp::this_shard(), 0);

        let Some(shard) = self.workers.borrow().get(&endpoint_id).cloned() else {
            return;
        };

        self.handle
            .invoke_on(shard, move |h| {
                h.local().destroy_worker(endpoint_id);
                std::future::ready(())
            })
            .await;

        self.num_workers[shard].set(self.num_workers[shard].get() - 1);
        self.workers.borrow_mut().remove(&endpoint_id);

        // Note: removing endpoints may create imbalance of worker distribution across shards.
        // Right now we don't do anything with it, as we don't expect huge number of workers,
        // and if new workers are added eventually, balance will be restored.
        // Alternatively we could migrate running workers among shards but it's probably not worth it.
    }

    fn create_worker(&self, endpoint_id: Uuid) {
        let mut shard_workers = self.shard_workers.borrow_mut();
        let mut listeners_liveness = self.listeners_liveness.borrow_mut();

        // Check that the endpoints does not appear in any of the following fields
        assert!(!shard_workers.contains_key(&endpoint_id));
        assert!(!listeners_liveness
            .iter()
            .any(|(_, ll)| ll.endpoint_liveness.contains_key(&endpoint_id)));

        // Proceed
        shard_workers.insert(endpoint_id, EndpointWorker::new());
        for (_, ll) in listeners_liveness.iter_mut() {
            ll.endpoint_liveness.insert(endpoint_id, Default::default());
        }

        // TODO: remember to spawn fibers
    }

    fn destroy_worker(&self, endpoint_id: Uuid) {
        let mut shard_workers = self.shard_workers.borrow_mut();
        let mut listeners_liveness = self.listeners_liveness.borrow_mut();
        let w = shard_workers
            .get(&endpoint_id)
            .expect("shard worker not found");

        // TODO: request abort and wait for the ping fiber

        // Mark the endpoint dead for all listeners which still consider it alive.
        // ping_fiber() is running no more so it's safe to adjust the `alive` flags.
        for (_, ll) in listeners_liveness.iter_mut() {
            ll.endpoint_liveness.get_mut(&endpoint_id).unwrap().alive = false;
        }
        // w.alive_changed.signal(); // TODO: wait for the notify fiber

        for (_, ll) in listeners_liveness.iter_mut() {
            ll.endpoint_liveness.remove(&endpoint_id);
        }
        shard_workers.remove(&endpoint_id);
    }

    async fn mark(&self, listener_id: ListenerId, endpoint_id: EndpointId, alive: bool) {
        let listener = self.local_listeners.borrow()[&listener_id];

        // Check if the listener is still registered by the time we received the notification.
        if !self.registered.borrow().contains(&(listener as *const _)) {
            return;
        }

        if alive {
            listener.mark_alive(endpoint_id).await;
        } else {
            listener.mark_dead(endpoint_id).await;
        }
    }
}

impl RustShardable for FailureDetector {
    type ConstructArgs = ();

    fn construct(
        handle: &ShardedHandle<Self>,
        _args: &Self::ConstructArgs,
    ) -> LocalBoxFuture<'static, Self> {
        let handle = Rc::new(handle.clone());
        let local = Self::new_local(handle);
        Box::pin(async move { local })
    }

    fn stop(&self) -> LocalBoxFuture<()> {
        if smp::this_shard() != 0 {
            // Shard 0 coordinates the stop.
            return Box::pin(async {});
        }

        let me = self.handle.local();
        Box::pin(async move {
            // TODO: Break the CV

            // TODO: Implement
        })
    }
}
