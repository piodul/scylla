/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// #[cxx::bridge(namespace = "direct_failure_detector")]
// mod ffi {
//     extern "C++" {
//         #[cxx_name = "pinger"]
//         type Pinger;

//     }
// }
use seastar::future::cpp::{CppBoolFuture, CppBoolPromise};
