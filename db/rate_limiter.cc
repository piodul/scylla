/*
 * Copyright (C) 2022-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <array>
#include <seastar/core/metrics.hh>

#include "utils/murmur_hash.hh"
#include "db/rate_limiter.hh"

namespace db {

static constexpr size_t hash_bits = 16;
static constexpr size_t bucket_count = 1 << hash_bits;
static constexpr size_t window_size = 10000;

void rate_limiter_base::on_timer() noexcept {
    _metrics.load_factor = double(_current_allocations_in_generation) / double(bucket_count);

    if (_current_allocations_in_generation == 0) {
        // No reads/writes happened since the last generation change,
        // so no need to switch to the next generation.
        return;
    }

    _current_window = 0;
    _current_allocations_in_generation = 0;
    _current_ops_in_window = 0;

    // Labels have 32 bits and are issued sequentially, so there is some risk
    // that it will wrap around. This may result in some very old labels
    // being incorrectly interpreted again as valid.
    //
    // In order to prevent this, buckets with invalid labels can be regularly
    // updated so that they are assigned a different, invalid label which
    // is further from the wraparound. Considering that the number of available
    // labels is much larger than a possible number of operations within
    // a generation, this can be done infrequently.
    //
    // One obvious way to do this would be e.g. to perform an update of all
    // buckets twice during the wraparound period (at 2^31 and 2^32).
    // Instead, the scan is amortized over time and partially performed
    // during each generation change.
    //
    // The range of available labels is large compared to the bucket count,
    // with current parameters we perform a bucket update
    // every 2^(32 - 16) / 2 = 2^15 labels assigned.

    constexpr size_t shift = 32 - hash_bits - 1;
    constexpr size_t mask = (1 << (32 - shift)) - 1;

    const size_t invalidate_begin = _first_active_label >> shift;
    const size_t invalidate_end = _next_label >> shift;

    for (size_t i = invalidate_begin; i != invalidate_end; i = (i + 1) & mask) {
        // We are changing the generation now, so all buckets are considered empty.
        _buckets[i % bucket_count].label = _next_label - 1;
    }

    // Invalidate all labels from the previous interval and start issuing again.
    _first_active_label = _next_label;
    _current_generation++;
}

rate_limiter_base::bucket* rate_limiter_base::get_bucket(uint32_t label, uint64_t token) noexcept {
    // We need to either find the existing bucket for this (label, token) combination
    // or otherwise find an invalid bucket which we can initialize and use.
    //
    // We start by looking at the bucket corresponding to the computed hash,
    // if it's occupied by another (label, token) try other buckets using
    // the quadratic probing strategy.
    //
    // We limit ourselves to 32 attempts - if no suitable bucket is found
    // then we return nullptr and admit the operation unconditionally.

    size_t hash = compute_hash(label, token);
    bucket* expired_candidate = nullptr;

    auto initialize = [&] (bucket& b) {
        b.token = token;
        b.label = label;
        b.op_count = _current_window;
    };

    static constexpr size_t max_probes = 32;
    for (size_t i = 0; i < max_probes; i++) {
        // Quadratic probing - every iteration jumps farther than the previous one
        hash = (hash + i) % bucket_count;
        bucket& b = _buckets[hash];
        ++_metrics.probe_count;

        if (bucket_is_empty(b)) {
            // We encountered an empty bucket, i.e. it was not initialized
            // within this generation and is not used for counting any other
            // (label, token).
            //
            // If we already have a candidate - which is an expired bucket - then
            // prefer it because accessing it requires less probes.
            // Initialize the chosen bucket and return it.
            if (!expired_candidate) {
                expired_candidate = &b;
                ++_metrics.allocations_on_empty;
                ++_current_allocations_in_generation;
                initialize(b);
                return &b;
            } else {
                ++_metrics.allocations_on_expired;
                initialize(*expired_candidate);
                return expired_candidate;
            }
        } else if (bucket_is_expired(b)) {
            // We encountered an expired bucket, i.e. it was initialized within
            // this generation but the lossy counting decremented it to zero
            // so it can be reused.
            //
            // We will keep track of the first-encountered expired bucket.
            // If we find the existing bucket for our (label, token) pair,
            // we will move it to the expired bucket in order to reduce
            // needed probes. If we won't, we will initialize and use
            // the expired bucket instead.
            if (!expired_candidate) {
                expired_candidate = &b;
            }
        } else if (b.token == token && b.label == label) {
            // We found our entry. If we found an expired bucket by the way,
            // relocate it there so that we perform less probes in the future.
            ++_metrics.successful_lookups;
            if (expired_candidate) {
                // Relocate the entry but make the old position non-empty
                // but expired. This is important in order not to break
                // probing for other entries.
                //
                // Setting `op_count` to 0 should do the trick.
                *expired_candidate = std::move(b);
                b.op_count = 0;
                ++_metrics.reallocations_onto_expired;
                return expired_candidate;
            }
            return &b;
        } else {
            // The current bucket is valid but already allocated for
            // a different (label, token) pair. Keep probing.
        }
    }

    // We didn't find our entry. This is our last chance: if there is an expired
    // candidate, initialize and use it, otherwise return nullptr.
    if (expired_candidate) {
        ++_metrics.allocations_on_expired;
        initialize(*expired_candidate);
        return expired_candidate;
    }
    ++_metrics.failed_allocations;
    return nullptr;
}

size_t rate_limiter_base::compute_hash(uint32_t label, uint64_t token) noexcept {
    // The map key is a tuple (token, key) + current generation as "salt"
    // The key is hashed with murmur hash for good hash quality

    static constexpr size_t key_length = sizeof(token) + sizeof(label) + sizeof(_current_generation);

    std::array<uint8_t, key_length> key;
    uint8_t* ptr = key.data();
    memcpy(ptr, &token, sizeof(token));
    ptr += sizeof(token);
    memcpy(ptr, &label, sizeof(label));
    ptr += sizeof(label);
    memcpy(ptr, &_current_generation, sizeof(_current_generation));

    std::array<uint64_t, 2> out;
    utils::murmur_hash::hash3_x64_128(key.data(), key_length, 0, out);
    return out[0];
}

bool rate_limiter_base::bucket_is_empty(const rate_limiter_base::bucket& b) noexcept {
    // The bucket is empty if its label was not assigned within this generation
    return b.label - _first_active_label >= _next_label - _first_active_label;
}

bool rate_limiter_base::bucket_is_expired(const rate_limiter_base::bucket& b) noexcept {
    // The bucket is expired if its operation count was decremented to 0 or below
    return b.op_count <= _current_window;
}

uint32_t rate_limiter_base::bucket_operation_count(const rate_limiter_base::bucket& b) noexcept {
    // Operation counts are virtually decremented by one every `window_size` operations
    // within this generation
    return b.op_count - _current_window;
}

void rate_limiter_base::register_metrics() {
    namespace sm = seastar::metrics;

    _metric_group.add_group("per_partition_rate_limiter", {
        // TODO: Most of the following metrics are pretty low-level and not useful for users,
        // perhaps they should be hidden behind a configuration flag

        sm::make_counter("allocations_on_empty", _metrics.allocations_on_empty,
                sm::description("Number of times a bucket was allocated on a bucket not yet allocated in this generation.")),
        
        sm::make_counter("allocations_on_expired", _metrics.allocations_on_expired,
                sm::description("Number of times a bucket was allocated over an expired bucket.")),
        
        sm::make_counter("reallocations_onto_expired", _metrics.reallocations_onto_expired,
                sm::description("Number of times an already allocated bucket was moved in order to reduce probe count.")),

        sm::make_counter("successful_lookups", _metrics.successful_lookups,
                sm::description("Number of times a lookup returned an already allocated bucket.")),
        
        sm::make_counter("failed_allocations", _metrics.failed_allocations,
                sm::description("Number of times the rate limiter gave up trying to allocate.")),

        sm::make_counter("probe_count", _metrics.probe_count,
                sm::description("Number of probes made during lookups.")),

        sm::make_gauge("load_factor", _metrics.load_factor,
                sm::description("Current load factor of the hash table.")),
    });
}

rate_limiter_base::rate_limiter_base()
        : _buckets(bucket_count, bucket{}) {
    
    register_metrics();
}

rate_limiter_base::can_proceed rate_limiter_base::account_operation(label& l, uint64_t token, uint64_t limit) noexcept {
    // If the label is no longer valid, refresh it
    if (l._generation != _current_generation) {
        l._generation = _current_generation;
        l._label = _next_label++;
    }

    bucket* b = get_bucket(l._label, token);
    if (!b) {
        // We failed to allocate a bucket for this partition. This means that
        // we won't track hit count for this partition during this generation.
        // Assume that it's OK to admit the operation.
        return can_proceed::yes;
    }
    if (bucket_operation_count(*b) + 1 <= limit) {
        ++b->op_count;
        ++_current_ops_in_window;
        if (_current_ops_in_window == window_size) {
            // Every `window_size` operations, virtually decrement all entries
            // by one. We implement it by always subtracting the `_current_window`
            // when comparing the count in the bucket with the limit.
            ++_current_window;
            _current_ops_in_window = 0;
        }
        return can_proceed::yes;
    } else {
        return can_proceed::no;
    }
}

template class generic_rate_limiter<seastar::lowres_clock>;

}
