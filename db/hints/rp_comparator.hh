/*
 * Copyright (C) 2021-present ScyllaDB
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

#include "db/commitlog/replay_position.hh"

namespace db {
namespace hints {

/// A replay position comparator which prioritizes segment IDs from other shards.
struct foreign_first_rp_comparator {
    const unsigned local_shard_id;

    inline bool operator()(const db::replay_position& a, const db::replay_position& b) const {
        const unsigned shard_a = a.shard_id();
        const unsigned shard_b = b.shard_id();

        if (shard_a == shard_b) {
            return a < b;
        }

        // Let S be the current shard, N - number of shards.
        // Put shards in the following order:
        //   (S + N - 1) % N
        //   (S + N - 2) % N
        //   ...
        //   (S + 1) % N
        //   S
        // This will, hopefully, prevent a situation in which hints managers from
        // all shards gang up on one shard and send hints to it at the same time.
        // Of course, nothing will help us if all shards have foreign segments
        // towards one shard only.

        // Instead of using modulo, we can use unsigned underflow. Resulting values
        // will have the same ordering as if modulo smp::count was used.
        return (shard_a - local_shard_id) > (shard_b - local_shard_id);
    }

    static inline db::replay_position min() {
        // Larger shard IDs are considered smaller (except for local_shard_id),
        // so use a large, fake shard ID
        return db::replay_position((1 << db::replay_position::max_cpu_bits) - 1, 0, 0);
    }

    explicit foreign_first_rp_comparator(unsigned shard_id) : local_shard_id(shard_id) {}
};

struct foreign_first_segment_id_comparator {
    const unsigned local_shard_id;

    inline bool operator()(const db::segment_id_type& a, const db::segment_id_type& b) const {
        return foreign_first_rp_comparator(local_shard_id)(db::replay_position(a), db::replay_position(b));
    }

    static inline db::segment_id_type min() {
        return foreign_first_rp_comparator::min().pos;
    }

    explicit foreign_first_segment_id_comparator(unsigned shard_id) : local_shard_id(shard_id) {}
};

}
}
