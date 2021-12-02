/*
 * Copyright (C) 2022-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <cstdint>
#include <seastar/core/manual_clock.hh>
#include <seastar/testing/test_case.hh>
#include "db/rate_limiter.hh"

using namespace seastar;
using test_rate_limiter = db::generic_rate_limiter<seastar::manual_clock>;

SEASTAR_TEST_CASE(test_rate_limiter_no_false_rejections) {
    const uint64_t key_set_size = 1000 * 1000;
    const uint64_t limit = 1;
    test_rate_limiter::label lbl;

    test_rate_limiter limiter;

    for (uint64_t token = 0; token < key_set_size; token++) {
        BOOST_REQUIRE(bool(limiter.account_operation(lbl, token, limit)));
    }

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_rate_limiter_reject_over_limit) {
    const size_t limit = 100;
    const uint64_t token = 123;
    test_rate_limiter::label lbl;

    test_rate_limiter limiter;

    for (size_t i = 0; i < limit; i++) {
        BOOST_REQUIRE(bool(limiter.account_operation(lbl, token, limit)));
    }
    for (size_t i = 0; i < limit; i++) {
        BOOST_REQUIRE(!bool(limiter.account_operation(lbl, token, limit)));
    }

    // Advance the time, the limiter should start accepting again
    manual_clock::advance(std::chrono::seconds(2));

    for (size_t i = 0; i < limit; i++) {
        BOOST_REQUIRE(bool(limiter.account_operation(lbl, token, limit)));
    }
    for (size_t i = 0; i < limit; i++) {
        BOOST_REQUIRE(!bool(limiter.account_operation(lbl, token, limit)));
    }

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_rate_limiter_label_wraparound_prevention) {
    const uint64_t token = 123;

    test_rate_limiter limiter;

    // Issue a label and do some operations
    test_rate_limiter::label lbl1;
    BOOST_REQUIRE_EQUAL(limiter.account_operation(lbl1, token, 1), test_rate_limiter::can_proceed::yes);
    BOOST_REQUIRE_EQUAL(limiter.account_operation(lbl1, token, 1), test_rate_limiter::can_proceed::no);

    // Simulate a lot of labels being issued, until they wrap around
    // Do it in several steps
    limiter.advance_labels((1 << 29) - 1);
    manual_clock::advance(std::chrono::seconds(2));

    for (int i = 0; i < 7; i++) {
        limiter.advance_labels(1 << 29);
        manual_clock::advance(std::chrono::seconds(2));
    }

    // Due to wraparound, we should get the same label again
    // However, at this point, the on_timer callback should make sure
    // that the problematic entries are cleared
    test_rate_limiter::label lbl2;
    BOOST_REQUIRE_EQUAL(limiter.account_operation(lbl2, token, 1), test_rate_limiter::can_proceed::yes);
    BOOST_REQUIRE_EQUAL(limiter.account_operation(lbl2, token, 1), test_rate_limiter::can_proceed::no);

    BOOST_REQUIRE_EQUAL(lbl1.get_label_id(), lbl2.get_label_id());

    return make_ready_future<>();
}
