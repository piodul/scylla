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

#include <cstdint>
#include <list>
#include <optional>
#include <seastar/core/abort_source.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sstring.hh>
#include "schema.hh"
#include "frozen_mutation.hh"
#include "db/commitlog/commitlog.hh"
#include "db/commitlog/replay_position.hh"

#include "seastarx.hh"

namespace db {
namespace hints {

// A store for hints targeted at a specific endpoint.
class queue_store {
private:
    struct segment {
        // TODO: Consider fs::path
        sstring path;
        db::segment_id_type segment_id;
    };

    struct segment_in_progress {
        segment seg;
        std::unordered_map<table_schema_version, column_mapping> schema_ver_to_column_mapping;
        db::position_type replayed_before_position = 0;
    };

private:
    const sstring _hints_dir;

    shared_future<lw_shared_ptr<commitlog>> _store;

    // TODO: Explain the segment lists

    std::list<segment> _segments_to_replay;
    std::optional<segment_in_progress> _currently_replayed_segment;
    std::list<segment> _unconfirmed_segments;

    seastar::condition_variable _new_segments_to_replay;

    future<> _flusher = make_ready_future<>();
    future<> _deletions = make_ready_future<>();

    bool _may_have_untracked_segments = true;

    seastar::gate _ops_gate;
    seastar::abort_source _as;

public:
    queue_store(sstring hints_dir)
            : _hints_dir(std::move(hints_dir))
    {}

    future<> start();
    future<> stop();

    future<> store_hint(schema_ptr s, lw_shared_ptr<const frozen_mutation> fm, tracing::trace_state_ptr tr_state);

    bool has_hints_to_replay() const {
        return !_segments_to_replay.empty() || _currently_replayed_segment.has_value();
    }
    future<> wait_for_hints_available_for_replay();
};

}
}
