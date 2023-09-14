/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <type_traits>
#include <seastar/core/future.hh>
#include "rust/cxx/future.hh"

namespace seastar::rs::internal {

template<typename T>
void promise_set_value(::seastar::promise<T>* prom, T* value) noexcept {
    if constexpr (!std::is_void_v<T>) {
        prom->set_value(std::move(*value));
        value->~T();
    } else {
        prom->set_value();
    }
}

template<typename T>
void promise_set_exception(::seastar::promise<T>* prom, std::exception_ptr* eptr) noexcept {
    prom->set_exception(std::move(*eptr));
}

template<typename T>
void* promise_get_future(::seastar::promise<T>* prom) noexcept {
    return new ::seastar::future<T>(prom->get_future());
}

} // namespace internal
