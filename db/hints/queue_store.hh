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
#include <map>
#include <optional>
#include <filesystem>
#include <seastar/core/abort_source.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/shared_mutex.hh>
#include "schema.hh"
#include "frozen_mutation.hh"
#include "db/commitlog/commitlog.hh"
#include "db/commitlog/replay_position.hh"
#include "db/hints/rp_comparator.hh"

#include "seastarx.hh"

namespace db {
namespace hints {

struct manager_stats;

// A store for hints targeted at a specific endpoint.
// TODO: A detailed explanation
class queue_store {
public:
    class reader;

private:
    struct segment {
        sstring path;
        db::segment_id_type segment_id;
    };
    struct segment_in_progress {
    public:
        segment seg;
        std::unordered_map<table_schema_version, column_mapping> schema_ver_to_column_mapping;
        db::position_type read_up_to = 0;

    private:
        frozen_mutation_and_schema decode_mutation(const database& db, fragmented_temporary_buffer& buf);
        const column_mapping& get_column_mapping(const frozen_mutation& fm, const commitlog_entry_reader& hr);

    public:
        segment_in_progress() {}

        /// Reads mutations sequentially starting from `read_up_to`.
        future<stop_iteration> read_mutations(const database& db, abort_source& as, gc_clock::duration secs_since_file_mod, reader& r, manager_stats& stats, bool do_drain);
    };

public:
    class reader {
    public:
        /// Called when the queue store is about to switch to another file for reading
        /// (or continue sending a previously paused segment)
        virtual future<stop_iteration> on_file_start(db::segment_id_type segment_id, const sstring& file_name, bool is_resuming) noexcept = 0;

        /// Called when the queue store has read all hints from the current file.
        virtual future<stop_iteration> on_file_end() = 0;

        /// Called when a mutation was read.
        virtual future<stop_iteration> on_mutation(db::replay_position rp, frozen_mutation_and_schema fm_a_s) noexcept = 0;
    };

private:
    const std::filesystem::path _hints_dir;

    shared_future<lw_shared_ptr<commitlog>> _store;

    // An ordered list of segments which we didn't start sending yet
    std::list<segment> _segments_to_replay;

    // Contains information about the segment being currently replayed
    std::optional<segment_in_progress> _currently_replayed_segment;

    // Contains segments which were read in full and are waiting to be manually deleted
    std::map<db::segment_id_type, sstring, foreign_first_segment_id_comparator> _segments_to_confirm;

    // Position of the most recently stored hint
    db::replay_position _last_stored_rp;

    // The segment ID up to which all hints were flushed
    db::segment_id_type _flushed_up_to_segment_id;

    // Triggered when _segments_to_replay becomes non-empty
    seastar::condition_variable _new_segments_to_replay;

    future<> _flusher = make_ready_future<>();
    bool _sending_in_progress = false;

    manager_stats& _stats;

    seastar::shared_mutex& _file_update_mutex;
    database& _local_db; // used to get commitlog extensions

    seastar::gate _ops_gate;
    seastar::abort_source _as;

    gms::inet_address _ep;
    unsigned _shard_id;
    uint64_t _stores_in_progress = 0;

    std::multimap<db::replay_position, lw_shared_ptr<std::optional<promise<>>>> _replay_waiters;

private:
    future<> run_flush_loop(lowres_clock::duration period);

    future<lw_shared_ptr<commitlog>> create_store();
    future<> delete_segment(const sstring& fname);

    void notify_replay_waiters() noexcept;
    void dismiss_replay_waiters() noexcept;

    bool has_foreign_segments() const noexcept;

public:
    queue_store(gms::inet_address ep, std::filesystem::path hints_dir,
            seastar::shared_mutex& file_update_mutex, database& local_db, manager_stats& stats,
            unsigned shard_id = this_shard_id());
    ~queue_store();

    void start(lowres_clock::duration flush_period = std::chrono::seconds(10));
    future<> stop();

    /// Makes sure that all hints written so far are persisted on disk
    future<> flush();

    bool store_hint(schema_ptr s, lw_shared_ptr<const frozen_mutation> fm, tracing::trace_state_ptr tr_state) noexcept;

    /// Wait until there are more hints available for replay.
    /// The abort_source can be used to cancel waiting.
    future<> wait_for_hints_available_for_replay(abort_source& as);

    /// Read hints from the queue, starting from the current position
    /// until an error or the end of the queue is encountered.
    /// The reader can also cause sending to be stopped if stop_iteration::yes
    /// is returned.
    /// This function must not be called concurrently with itself, `drain` or `roll_back_to`.
    future<> read(reader& r);

    /// Read hints from the queue with the goal of clearing it out.
    /// All errors and stop_iteration::yes are ignored.
    /// Segments are unconditionally deleted after all hints are read.
    /// The function will return only after the end of the queue is reached.
    /// This function must not be called concurrently with itself, `read` or `roll_back_to`.
    /// TODO: It would be the best to call this function on stop, in some cases
    future<> drain(reader& r);

    /// Rolls back hint replay to given position.
    /// The function is allowed to roll back to an even earlier position,
    /// for example when going back to a previously sent segment.
    /// This must not be called when `read` or `drain` is in progress.
    void roll_back_to(db::replay_position rp);

    /// Delete segments up to the segment with a given ID.
    /// This should be called when the sender is confident that hints up to
    /// the file were persisted on the other side.
    future<> confirm_flushed_up_to(db::segment_id_type segment_id_up_to);

    /// Waits until hints are confirmed to be flushed up to a given point.
    /// The abort_source can be used to cancel the waiting process.
    /// If the point has already been reached, the future resolves immediately,
    /// regardless of the status of the abort_source.
    future<> wait_until_hints_are_flushed_up_to(abort_source& as, db::replay_position up_to_rp);

    /// Returns the position of the most recently written hint.
    /// If there were no hints written during the queue's lifetime,
    /// it will return a fake position which is guaranteed to be before
    /// any later hint positions.
    inline db::replay_position last_stored_replay_position() const {
        return _last_stored_rp;
    }

    bool has_hints_to_replay() const {
        return !_segments_to_replay.empty() || _currently_replayed_segment.has_value();
    }

    /// Returns the number of hints that are being stored at the moment.
    inline uint64_t stores_in_progress() const {
        return _stores_in_progress;
    }

    const std::filesystem::path& hints_dir() const noexcept {
        return _hints_dir;
    }
};

}
}
