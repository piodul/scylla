/*
 * Copyright (C) 2022-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <exception>

#include <seastar/core/coroutine.hh>
#include <seastar/core/abort_source.hh>
#include <seastar/testing/test_case.hh>

#include "rust/inc.hh"
#include "rust/seastar/test.hh"

#include "rust/cxx/exception.hh"

SEASTAR_TEST_CASE(test_inc) {
    int k = 1;
    BOOST_REQUIRE(rust::inc(k) == 2);

    co_return;
}

SEASTAR_TEST_CASE(test_seastar_1) {
    auto fut = seastar::rs::test::test1(true);
    BOOST_REQUIRE(fut->available());
    BOOST_REQUIRE_EQUAL(fut->get(), true);

    fut = seastar::rs::test::test1(false);
    BOOST_REQUIRE(fut->available());
    BOOST_REQUIRE_EQUAL(fut->get(), false);

    co_return;
}

SEASTAR_TEST_CASE(test_seastar_2) {
    {
        seastar::promise<bool> p;
        seastar::future<bool> f = p.get_future();

        BOOST_REQUIRE(!f.available());
        seastar::rs::test::test2(seastar::rs::promise_box(std::move(p)), true);
        BOOST_REQUIRE(f.available());
        BOOST_REQUIRE_EQUAL(f.get(), true);
    }

    {
        seastar::promise<bool> p;
        seastar::future<bool> f = p.get_future();

        BOOST_REQUIRE(!f.available());
        seastar::rs::test::test2(seastar::rs::promise_box(std::move(p)), false);
        BOOST_REQUIRE(f.available());
        BOOST_REQUIRE_EQUAL(f.get(), false);
    }

    co_return;
}

SEASTAR_TEST_CASE(test_seastar_3) {
    auto fut = seastar::rs::test::test3(true);
    BOOST_REQUIRE(!fut->available());
    BOOST_REQUIRE_EQUAL(co_await std::move(*fut), true);

    co_return;
}

SEASTAR_TEST_CASE(test_seastar_4) {
    seastar::promise<uint32_t> p;
    auto fut = seastar::rs::test::test4(seastar::rs::future_box(p.get_future()));

    p.set_value(123);
    BOOST_REQUIRE_EQUAL(co_await std::move(*fut), 246);

    co_return;
}

SEASTAR_TEST_CASE(test_seastar_5) {
    auto eptr = std::make_exception_ptr(seastar::abort_requested_exception());
    BOOST_REQUIRE_EQUAL(seastar::rs::test::test5(eptr), "ok");

    co_return;
}

SEASTAR_TEST_CASE(test_seastar_panics_in_rust_encoded_as_exception) {
    auto eptr = seastar::rs::test::instantiate_a_panic("ala ma kota");
    try {
        std::rethrow_exception(eptr);
    } catch (const rust_panic& rp) {
        BOOST_REQUIRE_EQUAL(rp.what(), std::string("ala ma kota"));
    }

    co_return;
}

SEASTAR_TEST_CASE(test_seastar_rust_decodes_a_panic_properly) {
    auto eptr = seastar::rs::test::instantiate_a_panic("ala ma kota");
    auto recovered = seastar::rs::test::consume_panic(eptr);
    BOOST_REQUIRE_EQUAL(recovered, "ala ma kota");

    co_return;
}

SEASTAR_TEST_CASE(test_seastar_rust_transparently_handles_exceptions) {
    auto f = seastar::make_exception_future<bool>(std::runtime_error("kot ma pchły"));
    auto f2 = seastar::rs::test::test_exception_repackaging(std::move(f));
    BOOST_REQUIRE_THROW(co_await std::move(*f2), std::runtime_error);

    co_return;
}

SEASTAR_TEST_CASE(test_seastar_rust_submit_to) {
    co_await std::move(*seastar::rs::test::test_submit_to());
}

SEASTAR_TEST_CASE(test_seastar_abort_source) {
    auto as = std::make_unique<seastar::abort_source>();
    co_await std::move(*seastar::rs::test::test_abort_source(std::move(as)));
}
