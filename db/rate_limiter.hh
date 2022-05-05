/*
 * Copyright (C) 2022-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <chrono>
#include <limits>
#include <concepts>
#include <vector>
#include <optional>
#include <random>

#include <seastar/core/future.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/util/bool_class.hh>

#include "utils/chunked_vector.hh"

// A data structure used to implement per-partition rate limiting.
//
// The rate limiter keeps a map of counters which keep track of how many
// times given token for given table/operation type was accessed. Counters
// are identified by a (label, token) pair, where `label` is an identifier
// assigned by the limiter to differentiate types of operations which should
// be counted separately (e.g. reads/writes, operations on different tables).
//
// Operations are counted within one second intervals. Every second,
// a "generation change" happens and all current counters are removed, lazily.
//
// In order to reduce the memory needed for counters in case of large throughput
// with large number of unique partitions, every 10k operations (called
// a window) all counters are decremented by 1. Memory used by those counters
// which go down to 0 or below can be then reused. This is also done lazily.
// The idea is called "lossy counting".
// (TODO: how much memory do we save vs. not doing it at all?)
//
// The implementation is based on a fixed-size hashmap with quadratic probing.
// Each hashmap bucket is identified by the (label, token) pair.
//
// The structure's parameters were chosen to easily support 200k operations
// per second in the worst case. It takes up about 1.25MB per shard and supports
// counting operations from multiple tables at once.
//
// All operations are O(1). The structure maintains a timer which wakes up
// every second and performs a quick maintenance operation.

namespace db {

class rate_limiter_base {
private:
    struct metrics {
        uint64_t allocations_on_empty = 0;
        uint64_t allocations_on_expired = 0;
        uint64_t reallocations_onto_expired = 0;
        uint64_t successful_lookups = 0;
        uint64_t failed_allocations = 0;
        uint64_t probe_count = 0;
        double load_factor = 0.0;
    };

    // Represents a piece of the hashmap storage.
    struct bucket {
    public:
        // The partition key token of the operation which allocated this bucket.
        uint64_t token = 0;

        // The label of the operation which allocated this bucket.
        // Labels are used to differentiate operations which should be counted
        // separately, e.g. reads and writes to the same table or writes
        // to two different tables.
        // Labels are invalidated on each generation change (every second).
        // A bucket with invalid generation is considered to be "empty"
        // and may be overwritten by another operation.
        uint32_t label = 0;

        // The number of operations counted for given token/label.
        // It is virtually decremented on each window change, so the real
        // operation count is actually `op_count - _current_window`.
        // If the number drops to zero or below, the bucket is considered
        // "expired" and may be overwritten by another operation.
        uint32_t op_count = 0;

        uint32_t generation = 0;
    };

public:
    struct can_proceed_tag{};
    using can_proceed = seastar::bool_class<can_proceed_tag>;

    // Identifies a type of operation which is counted separately from other
    // operations. For example, reads and writes for given table should have
    // separate labels.
    struct label {
    private:
        // TODO: Label generations

        // The current ID used to identify the label in the rate limiter.
        // On generation switch, it becomes invalid and it is lazily reassigned.
        uint32_t _label = 0;

        friend class rate_limiter_base;

    public:
        // For testing purposes only
        uint32_t get_label_id() const {
            return _label;
        }
    };

private:
    uint32_t _current_window = 0;
    uint32_t _current_allocations_in_generation = 0;
    uint32_t _current_ops_in_window = 0;

    uint32_t _next_label = 1;
    uint32_t _first_active_label = 1;
    uint32_t _current_generation = 0;

    std::default_random_engine _random;
    const uint32_t _salt;

    // TODO: We know in compile time how many buckets are there,
    // so it might be more efficient to split into chunks manually
    utils::chunked_vector<bucket> _buckets;

    metrics _metrics;
    seastar::metrics::metric_groups _metric_group;

private:
    bucket* get_bucket(uint32_t table, uint64_t token) noexcept;
    size_t compute_hash(uint32_t label, uint64_t token) noexcept;

    void bucket_refresh(bucket& b) noexcept;
    bool bucket_is_empty(const bucket& b) noexcept;
    bool bucket_is_expired(const bucket& b) noexcept;
    uint32_t bucket_operation_count(const bucket& b) noexcept;

    void register_metrics();

protected:
    void on_timer() noexcept;

public:
    rate_limiter_base();

    rate_limiter_base(const rate_limiter_base&) = delete;
    rate_limiter_base(rate_limiter_base&&) = delete;

    rate_limiter_base& operator=(const rate_limiter_base&) = delete;
    rate_limiter_base& operator=(rate_limiter_base&&) = delete;

    // If the counter corresponding to (label, token) pair is under the limit,
    // increments it and returns can_proceed::yes. Otherwise, returns can_proceed::no;
    can_proceed account_operation(label& l, uint64_t token, uint64_t limit) noexcept;

    inline const metrics& get_metrics() const noexcept {
        return _metrics;
    }

    // For testing purposes only
    void advance_labels(uint32_t by) {
        _next_label += by;
    }
};

template<typename ClockType>
class generic_rate_limiter : public rate_limiter_base {
private:
    seastar::timer<ClockType> _timer;

public:
    generic_rate_limiter()
            : rate_limiter_base() {

        _timer.set_callback([this] { on_timer(); });
        _timer.arm_periodic(std::chrono::seconds(1));
    }
};

extern template class generic_rate_limiter<seastar::lowres_clock>;
using rate_limiter = generic_rate_limiter<seastar::lowres_clock>;

}
