/*
 * Copyright (C) 2020 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <array>
#include <cstdint>
#include <seastar/core/metrics_registration.hh>
#include "enum_set.hh"
#include "utils/histogram.hh"
#include "utils/estimated_histogram.hh"

namespace cdc {

struct stats {
private:
    seastar::metrics::metric_groups _metrics;

public:
    enum class part_type {
        STATIC_ROW,
        CLUSTERING_ROW,
        MAP,
        SET,
        LIST,
        UDT,
        RANGE_TOMBSTONE,
        PARTITION_DELETE,
        ROW_DELETE,

        MAX
    };

    using part_type_set = enum_set<super_enum<part_type,
        part_type::STATIC_ROW,
        part_type::CLUSTERING_ROW,
        part_type::MAP,
        part_type::SET,
        part_type::LIST,
        part_type::UDT,
        part_type::RANGE_TOMBSTONE,
        part_type::PARTITION_DELETE,
        part_type::ROW_DELETE
    >>;

    struct parts_touched_stats {
        std::array<uint64_t, (size_t)part_type::MAX> count = {};

        inline void apply(part_type_set parts_set) {
            for (part_type idx : parts_set) {
                count[(size_t)idx]++;
            }
        }

        void register_metrics(seastar::metrics::metric_groups& metrics, sstring suffix);
    };

    struct counters {
        uint64_t unsplit_count = 0;
        uint64_t split_count = 0;
        uint64_t preimage_selects = 0;

        parts_touched_stats touches;
    };

    counters counters_total;
    counters counters_failed;

    stats();
};

}
