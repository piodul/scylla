/*
 * Copyright (C) 2022-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

use utils::Uuid;

#[cxx::bridge]
mod ffi {
    #[namespace = "seastar::rs::generated"]
    extern "C++" {
        type BoxFutureUnit = seastar::BoxFutureUnit;
    }

    #[namespace = "utils"]
    extern "C++" {
        type Uuid = utils::Uuid;
    }

    #[namespace = "direct_failure_detector"]
    unsafe extern "C++" {
        include!("direct_failure_detector/failure_detector.hh");

        #[cxx_name = "clock"]
        type Clock;
        fn now(self: &Clock) -> i64;
        fn sleep_until(self: &Clock, tp: i64) -> BoxFutureUnit; // TODO: AbortSource

        #[cxx_name = "listener"]
        type Listener;
        fn mark_alive(endpoint_id: Uuid) -> BoxFutureUnit;
        fn mark_dead(endpoint_id: Uuid) -> BoxFutureUnit;
    }

    #[namespace = "direct_failure_detector"]
    extern "Rust" {
        #[cxx_name = "subscription"]
        type Subscription;
    }

    // Direct failure detector will have to be exposed via some codegen, with Sharded<>?
}

struct Subscription {
    // ?
}

struct FailureDetector {
    // ?
}

/// Public interface
impl FailureDetector {
    #[seastar::taskify]
    pub async fn register_listener(&self, endpoint_id: Uuid) {
        todo!()
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
