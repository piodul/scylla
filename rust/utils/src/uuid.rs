/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/// Corresponds to the utils::UUID type defined in C++.
#[repr(C)]
#[derive(PartialEq, Eq, PartialOrd, Ord, Debug, Clone, Copy, Hash)]
pub struct Uuid {
    most_sig_bits: i64,
    least_sig_bits: i64,
}

unsafe impl cxx::ExternType for Uuid {
    type Id = cxx::type_id!(utils::Uuid);
    type Kind = cxx::kind::Trivial;
}

// TODO: Add methods as they become needed
