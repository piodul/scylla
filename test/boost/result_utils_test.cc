/*
 * Copyright (C) 2022-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <vector>
#include <stdexcept>
#include "utils/exception_container.hh"
#include "utils/result.hh"

#include <seastar/testing/test_case.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/map_reduce.hh>
#include <seastar/testing/thread_test_case.hh>

using namespace seastar;
namespace bo = BOOST_OUTCOME_V2_NAMESPACE;

class foo_exception : public std::exception {
public:
    const char* what() const noexcept override {
        return "foo";
    }
};

class bar_exception : public std::exception {
public:
    const char* what() const noexcept override {
        return "bar";
    }
};

using exc_container = utils::exception_container<foo_exception, bar_exception>;

template<typename T = void>
using result = bo::result<T, exc_container,utils::exception_container_throw_policy>;

SEASTAR_TEST_CASE(test_exception_container_throw_policy) {
    result<> r_ok = bo::success();
    BOOST_REQUIRE_NO_THROW(r_ok.value());
    BOOST_REQUIRE_THROW(r_ok.error(), bo::bad_result_access);

    result<> r_err_foo = bo::failure(foo_exception());
    BOOST_REQUIRE_NO_THROW(r_err_foo.error());
    BOOST_REQUIRE_THROW(r_err_foo.value(), foo_exception);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_result_into_future) {
    result<> r_ok = bo::success();
    auto f_ok = utils::result_into_future(std::move(r_ok));
    BOOST_REQUIRE_NO_THROW(f_ok.get());

    result<> r_err_foo = bo::failure(foo_exception());
    auto f_err_foo = utils::result_into_future(std::move(r_err_foo));
    BOOST_REQUIRE_THROW(f_err_foo.get(), foo_exception);

    return make_ready_future<>();
}

SEASTAR_THREAD_TEST_CASE(test_result_reducer) {
    auto reduce = [] (auto... params) {
        std::vector<result<>> v;
        (v.push_back(std::move(params)), ...);
        map_reduce(
            std::move(v),
            [] (result<>& r) { return make_ready_future<result<>>(std::move(r)); },
            result<>(bo::success()),
            utils::result_reducer{}
        ).get().value(); // <- trying to access the value throws in case of error
    };

    auto foo_exc = [] () { return result<>(bo::failure(foo_exception())); };
    auto bar_exc = [] () { return result<>(bo::failure(bar_exception())); };

    BOOST_REQUIRE_NO_THROW(reduce(bo::success(), bo::success()));
    BOOST_REQUIRE_THROW(reduce(foo_exc(), bo::success()), foo_exception);
    BOOST_REQUIRE_THROW(reduce(bo::success(), foo_exc()), foo_exception);
    BOOST_REQUIRE_THROW(reduce(foo_exc(), bar_exc()), foo_exception);
    BOOST_REQUIRE_THROW(reduce(bar_exc(), foo_exc()), bar_exception);
}
