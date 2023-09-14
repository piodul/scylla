/*
 * Copyright 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <exception>

#include "rust/cxx.h"

/// A C++ exception that represents a Rust panic.
///
/// Rust panics, unless they abort the process immediately, are propagated via unwinding, similarly to C++ exceptions.
/// They are not true C++ exceptions, i.e. unwinding from Rust to C++ is undefined behavior, so in order to propagate
/// them through C++ we need to use a wrapper like this one.
///
/// Unlike C++ exceptions, Rust panics are not (ab)used for general error handling and are reserved for truly
/// exceptional situations which sometimes need to be properly handled, e.g. for failed assertions.
///
/// Unfortunately, in C++ exceptions can be handled through std::exception_ptr which has shared pointer semantics,
/// whereas panic payloads are handled through Box<dyn Any + Send> which has unique pointer semantics and cannot
/// in general be copied. Because of that, we restrict ourselves only to String/&str types which are thrown by `panic!`
/// macro and others are unsupported.
///
/// TODO: Should this inherit from std::runtime_error instead of encapsulating it?
class rust_panic final : public std::exception {
private:
    // Encapsulating a runtime_error for its copy-on-write string
    std::runtime_error _impl;

public:
    rust_panic(const char* what) : _impl(what) {}

    rust_panic(rust_panic&&) = default;
    rust_panic(const rust_panic&) = default;

    rust_panic& operator=(rust_panic&&) = default;
    rust_panic& operator=(const rust_panic&) = default;

    const char* what() const noexcept override {
        return _impl.what();
    }
};

// TODO: Convince myself that this works; this is a shared pointer and shared pointers do work (although natively)
// with cxx; moreover, cxx-async also uses a similar trick for the futures, therefore this _should_ work nicely.
template<> struct rust::IsRelocatable<std::exception_ptr> : std::true_type {};
