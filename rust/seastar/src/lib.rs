/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

pub mod exception;
pub mod foreign;
pub mod future;
pub mod promise;
pub mod sched;
pub mod sharded;
pub mod smp;
pub mod task;
pub mod test;

pub mod native;

pub use future::BoxFuture;

pub use seastar_macros::taskify;

seastar_macros::gen_py!("idl/futures_promises_primitive.idl.yaml");
