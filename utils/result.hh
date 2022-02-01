/*
 * Copyright 2022-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

// A collection of utilities related to boost::outcome::result.

#include <boost/outcome/policy/base.hpp>
#include <boost/outcome/result.hpp>
#include <seastar/core/future.hh>
#include "utils/exception_container.hh"

namespace utils {

namespace bo = BOOST_OUTCOME_V2_NAMESPACE;

// A policy which throws the container_error associated with the result
// if there was an attempt to access value while it was not present.
struct exception_container_throw_policy : bo::policy::base {
    template<class Impl> static constexpr void wide_value_check(Impl&& self) {
        if (!base::_has_value(self)) {
            base::_error(self).throw_me();
        }
    }

    template<class Impl> static constexpr void wide_error_check(Impl&& self) {
        if (!base::_has_error(self)) {
            throw bo::bad_result_access("no error");
        }
    }
};

// A reducer which can facilitate adding results to parallel_for_each loops
// by converting them to map_reduce.
//
// The reducer takes two results of the same type and returns success if both
// have a value, otherwise returns the leftmost error.
struct result_reducer {
    template<typename E, typename P>
    bo::result<void, E, P> operator()(bo::result<void, E, P>&& a, bo::result<void, E, P>&& b) {
        if (a.has_error() || !b.has_error()) {
            return std::move(a);
        } else {
            return std::move(b);
        }
    }
};

// Converts a result into either a ready or an exceptional future.
// Supports only results which has an exception_container as their error type.
template<typename R>
requires bo::is_basic_result<R>::value && ExceptionContainer<typename R::error_type>
seastar::future<typename R::value_type> result_into_future(R&& res) {
    if (res.has_error()) {
        return std::move(res).assume_error().template into_exception_future<typename R::value_type>();
    } else {
        if constexpr (std::is_void_v<typename R::value_type>) {
            return seastar::make_ready_future<>();
        } else {
            return seastar::make_ready_future<typename R::value_type>(std::move(res).assume_value());
        }
    }
}

}
