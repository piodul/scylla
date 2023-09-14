/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <exception>
#include <seastar/core/abort_source.hh>
#include <seastar/core/timed_out_error.hh>
#include "utils/exceptions.hh"
#include "exception.hh"

#include "rust/seastar/idl/exceptions_std.idl.hh"
#include "rust/seastar/idl/exceptions_std.dist.hh"
#include "rust/seastar/idl/exceptions_seastar.idl.hh"
#include "rust/seastar/idl/exceptions_seastar.dist.hh"

extern "C" void seastar_rs_exception_ptr_clone(const std::exception_ptr* src, std::exception_ptr* dst) noexcept {
    new (dst) std::exception_ptr(*src);
}

extern "C" void seastar_rs_exception_ptr_drop(std::exception_ptr* ptr) noexcept {
    ptr->~exception_ptr();
}

extern "C" const char* seastar_rs_exception_get_what(std::exception* ptr) noexcept {
    return ptr->what();
}

extern "C" void seastar_rs_exception_from_rust_panic(const char* what, std::exception_ptr* dst) noexcept {
    new (dst) std::exception_ptr(std::make_exception_ptr<rust_panic>(rust_panic(what)));
}

extern "C" const char* seastar_rs_exception_recover_panic_from_exception(const std::exception_ptr* eptr) noexcept {
    if (*eptr) {
        if (const auto* rp = try_catch<rust_panic>(*const_cast<std::exception_ptr*>(eptr))) [[unlikely]] {
            return rp->what();
        }
    }
    return nullptr;
}
