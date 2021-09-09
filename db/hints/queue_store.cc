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

#include <algorithm>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/file.hh>
#include <seastar/core/seastar.hh>
#include "database.hh"
#include "converting_mutation_partition_applier.hh"
#include "frozen_mutation.hh"
#include "mutation_partition_view.hh"
#include "service/priority_manager.hh"
#include "db/hints/queue_store.hh"
#include "db/hints/rp_comparator.hh"
#include "db/hints/resource_manager.hh"
#include "db/hints/manager.hh"
#include "utils/runtime.hh"
#include "utils/error_injection.hh"

namespace db {
namespace hints {

static const std::string FILENAME_PREFIX = "HintsLog" + commitlog::descriptor::SEPARATOR;
static const std::chrono::seconds hint_file_write_timeout = std::chrono::seconds(2);
static const std::chrono::seconds hints_flush_period = std::chrono::seconds(10);

static logging::logger queue_logger("hints_queue");

class no_column_mapping : public std::out_of_range {
public:
    no_column_mapping(const utils::UUID& id) : std::out_of_range(format("column mapping for CF {} is missing", id)) {}
};

queue_store::queue_store(gms::inet_address ep, std::filesystem::path hints_dir,
        seastar::shared_mutex& file_update_mutex, database& local_db, manager_stats& stats,
        unsigned shard_id)
        : _hints_dir(std::move(hints_dir))
        , _segments_to_confirm(foreign_first_segment_id_comparator{shard_id})
        // Approximate the position of the last written hint by using the same formula as for segment id calculation in commitlog
        // TODO: Should this logic be deduplicated with what is in the commitlog?
        , _last_stored_rp(this_shard_id(), std::chrono::duration_cast<std::chrono::milliseconds>(runtime::get_boot_time().time_since_epoch()).count())
        , _flushed_up_to_segment_id(foreign_first_segment_id_comparator::min())
        , _stats(stats)
        , _file_update_mutex(file_update_mutex)
        , _local_db(local_db)
        , _ep(ep)
{}

queue_store::~queue_store() {
}

void queue_store::start(lowres_clock::duration flush_period) {
    _store = shared_future<lw_shared_ptr<commitlog>>(create_store());
    // TODO: We need this to run in a designated scheduling group (streaming)
    _flusher = run_flush_loop(flush_period);
}

future<> queue_store::stop() {
    // Abort all asynchronous processes
    _as.request_abort();
    _new_segments_to_replay.broken();

    co_await std::move(_flusher);
    co_await _ops_gate.close();
    dismiss_replay_waiters();

    // TODO: Move destroying the commitlog into a separate function?
    auto old_store = co_await _store.get_future();
    co_await old_store->shutdown();
    co_await old_store->release();
    _store = shared_future<lw_shared_ptr<commitlog>>();
}

future<> queue_store::run_flush_loop(lowres_clock::duration period) {
    lowres_clock::time_point next_flush_time = lowres_clock::now() + period;
    queue_logger.debug("[{}] Starting the flush loop", _ep);
    while (true) {
        try {
            // Sleep at least 10 ticks of the clock
            auto now = lowres_clock::now();
            auto duration_to_sleep = std::max(lowres_clock::duration(10), next_flush_time - now);
            co_await sleep_abortable(duration_to_sleep, _as);

            next_flush_time = lowres_clock::now() + period;
            co_await flush();
        } catch (seastar::sleep_aborted&) {
            queue_logger.debug("[{}] Stopping the flush loop", _ep);
            break;
        } catch (...) {
            // Log the error and continue
            queue_logger.debug("error while trying to flush hints to disk: {}", std::current_exception());
        }
    }
}

future<> queue_store::flush() {
    // if (!_need_flush) {
    //     queue_logger.debug("[{}] Skipping flush because there were no hints written since the last flush", _ep);
    //     co_return;
    // }

    co_await _file_update_mutex.lock();
    auto unlock = defer([this] { _file_update_mutex.unlock(); });

    queue_logger.debug("[{}] Performing a flush", _ep);

    auto f_old_store = _store.get_future();

    // Capture `this` as `self_` in order not to accidentally use `this`
    _store = shared_future<lw_shared_ptr<commitlog>>(f_old_store.then([self_ = this] (lw_shared_ptr<commitlog> old_store) -> future<lw_shared_ptr<commitlog>> {
        // Preserve `this` in the coroutine frame as `self`
        auto self = self_;

        co_await old_store->shutdown();
        co_await old_store->release();
        old_store = nullptr;

        // If this fails, we are in a pickle... maybe we should retry in a loop?
        co_return co_await self->create_store();
    }));
    co_await _store.get_future().discard_result();
}

future<lw_shared_ptr<commitlog>> queue_store::create_store() {
    queue_logger.debug("Going to add a store to {}", _hints_dir.c_str());

    co_await io_check([name = _hints_dir.c_str()] { return recursive_touch_directory(name); });

    commitlog::config cfg;

    cfg.commit_log_location = _hints_dir.c_str();
    cfg.commitlog_segment_size_in_mb = resource_manager::hint_segment_size_in_mb;
    cfg.commitlog_total_space_in_mb = resource_manager::max_hints_per_ep_size_mb;
    cfg.fname_prefix = FILENAME_PREFIX;
    cfg.extensions = &_local_db.extensions();

    // HH doesn't utilize the flow that benefits from reusing segments.
    // Therefore let's simply disable it to avoid any possible confusion.
    cfg.reuse_segments = false;
    // HH leaves segments on disk after commitlog shutdown, and later reads
    // them when commitlog is re-created. This is expected to happen regularly
    // during standard HH workload, so no need to print a warning about it.
    cfg.warn_about_segments_left_on_disk_after_shutdown = false;
    // Allow going over the configured size limit of the commitlog
    // (resource_manager::max_hints_per_ep_size_mb). The commitlog will
    // be more conservative with its disk usage when going over the limit.
    // On the other hand, HH counts used space using the space_watchdog
    // in resource_manager, so its redundant for the commitlog to apply
    // a hard limit.
    cfg.allow_going_over_size_limit = true;

    commitlog l = co_await commitlog::create_commitlog(std::move(cfg));

    if (!has_hints_to_replay()) {
        // Re-populate the list only if we ran out of segments to replay

        std::vector<sstring> segs_vec = l.get_segments_to_replay();

        if (segs_vec.empty()) {
            // If the segs_vec is empty, this means that there are no more
            // hints to be replayed. We can safely skip to the position of the
            // last written hint.
            //
            // This is necessary: remember that we artificially set
            // the last replayed position based on the creation time
            // of the endpoint manager. If we replay all segments from
            // previous runtimes but won't write any new hints during
            // this runtime, then without the logic below the hint replay
            // tracker won't reach the hint written tracker.
            _flushed_up_to_segment_id = _last_stored_rp.id;
            notify_replay_waiters();
        }

        // Parse segment names and extract their segment IDs
        std::vector<std::pair<db::segment_id_type, sstring>> segs_with_ids;
        segs_with_ids.reserve(segs_vec.size());

        for (auto& seg : segs_vec) {
            db::commitlog::descriptor desc(seg, FILENAME_PREFIX);
            // The list reported by the commitlog will contain segments to confirm,
            // so we need to filter them out
            if (!_segments_to_confirm.contains(desc.id)) {
                segs_with_ids.emplace_back(desc.id, std::move(seg));
            }
        }

        // Sort segments by their segment IDs, starting from those
        // which are from foreign shards
        foreign_first_segment_id_comparator cmp(this_shard_id());
        std::sort(segs_with_ids.begin(), segs_with_ids.end(), [cmp] (const auto& a, const auto& b) {
            return cmp(a.first, b.first);
        });

        // Move the segments to the _segments_to_replay list
        for (auto& [segment_id, path] : segs_with_ids) {
            _segments_to_replay.push_back(segment {
                .path = std::move(path),
                .segment_id = segment_id,
            });
        }

        if (!_segments_to_replay.empty()) {
            // Notify those who wait in `wait_for_hints_available_for_replay()`
            _new_segments_to_replay.broadcast();
        } else if (!_segments_to_confirm.empty()) {
            auto rp_bound = db::replay_position(_segments_to_replay.front().segment_id - 1, std::numeric_limits<db::position_type>::max());
        }
    }
    
    co_return make_lw_shared<commitlog>(std::move(l));
}

bool queue_store::store_hint(schema_ptr s, lw_shared_ptr<const frozen_mutation> fm, tracing::trace_state_ptr tr_state) noexcept {
    try {
        // Future is waited on indirectly in `stop()` (via `_ops_gate`).
        (void)with_gate(_ops_gate, [this, s = std::move(s), fm = std::move(fm), tr_state] () mutable {
            ++_stores_in_progress;
            size_t mut_size = fm->representation().size();
            _stats.size_of_hints_in_progress += mut_size;

            return with_shared(_file_update_mutex, [this, fm, s, tr_state] () mutable -> future<> {
                return _store.get_future().then([this, fm = std::move(fm), s = std::move(s), tr_state] (lw_shared_ptr<commitlog> log_ptr) mutable {
                    commitlog_entry_writer cew(s, *fm, db::commitlog::force_sync::no);
                    return log_ptr->add_entry(s->id(), cew, db::timeout_clock::now() + hint_file_write_timeout);
                }).then([this, tr_state] (db::rp_handle rh) {
                    auto rp = rh.release();
                    foreign_first_rp_comparator cmp(this_shard_id());
                    if (cmp(_last_stored_rp, rp)) {
                        _last_stored_rp = rp;
                        queue_logger.debug("[{}] Updated last written replay position to {}", _ep, rp);
                    }
                    ++_stats.written;

                    queue_logger.trace("Hint to {} was stored", _ep);
                    tracing::trace(tr_state, "Hint to {} was stored", _ep);
                }).handle_exception([this, tr_state] (std::exception_ptr eptr) {
                    ++_stats.errors;

                    queue_logger.debug("store_hint(): got the exception when storing a hint to {}: {}", _ep, eptr);
                    tracing::trace(tr_state, "Failed to store a hint to {}: {}", _ep, eptr);
                });
            }).finally([this, mut_size, fm, s] {
                --_stores_in_progress;
                _stats.size_of_hints_in_progress -= mut_size;
            });
        });
    } catch (...) {
        queue_logger.trace("Failed to store a hint to {}: {}", _ep, std::current_exception());
        tracing::trace(tr_state, "Failed to store a hint to {}: {}", _ep, std::current_exception());

        ++_stats.dropped;
        return false;
    }
    return true;
}

future<> queue_store::wait_for_hints_available_for_replay(abort_source& as) {
    // Broadcast a signal on the condition variable in case abort is requested.
    // In practice there shouldn't be too many waiters on the condition variable
    // (at most one coming from hints endpoint manager), so notifying all waiters
    // shouldn't hurt.
    const auto subscription = as.subscribe([this] () noexcept {
        _new_segments_to_replay.broadcast();
    });

    while (!has_hints_to_replay()) {
        co_await _new_segments_to_replay.wait();
        if (as.abort_requested()) {
            throw abort_requested_exception();
        }
    }
}

static future<timespec> get_last_file_modification(const sstring& fname) {
    file f = co_await seastar::open_file_dma(fname, open_flags::ro);
    struct stat st = co_await f.stat();
    co_return st.st_mtim;
}

future<> queue_store::read(queue_store::reader& r) {
    if (_sending_in_progress) {
        throw std::runtime_error("queue_store::read called concurrently with other queue-modifying methods");
    }

    auto holder = _ops_gate.hold();

    _sending_in_progress = true;
    auto flag_off = defer([this] { _sending_in_progress = false; });

    int replayed_segments_count = 0;
    stop_iteration should_stop = stop_iteration::no;

    while (has_hints_to_replay()) {
        bool is_resuming = true;
        if (!_currently_replayed_segment.has_value()) {
            // Move to a new segment for sending
            _currently_replayed_segment.emplace();
            _currently_replayed_segment->seg = std::move(_segments_to_replay.front());
            _segments_to_replay.pop_front();
            is_resuming = false;
        }

        should_stop = co_await r.on_file_start(_currently_replayed_segment->seg.segment_id, _currently_replayed_segment->seg.path, is_resuming);
        if (should_stop) {
            co_return;
        }

        timespec last_mod = co_await get_last_file_modification(_currently_replayed_segment->seg.path);
        gc_clock::duration secs_since_file_mod = std::chrono::seconds(last_mod.tv_sec);

        should_stop = co_await _currently_replayed_segment->read_mutations(_local_db, _as, secs_since_file_mod, r, _stats, true);
        if (should_stop) {
            co_return;
        }

        _segments_to_confirm.try_emplace(_currently_replayed_segment->seg.segment_id, std::move(_currently_replayed_segment->seg.path));
        _currently_replayed_segment.reset();

        should_stop = co_await r.on_file_end();
        if (should_stop) {
            co_return;
        }
    }
}

future<> queue_store::drain(queue_store::reader& r) {
    if (_sending_in_progress) {
        throw std::runtime_error("queue_store::read called concurrently with other queue-modifying methods");
    }

    auto holder = _ops_gate.hold();

    _sending_in_progress = true;
    auto flag_off = defer([this] { _sending_in_progress = false; });

    // Delete files which were waiting for confirmation
    while (!_segments_to_confirm.empty()) {
        co_await delete_segment(_segments_to_confirm.begin()->second);
        _segments_to_confirm.erase(_segments_to_confirm.begin());
    }

    auto handle_next_file = [&] () -> future<> {
        bool is_resuming = true;
        if (!_currently_replayed_segment.has_value()) {
            // Move to a new segment for sending
            _currently_replayed_segment.emplace();
            _currently_replayed_segment->seg = std::move(_segments_to_replay.front());
            _segments_to_replay.pop_front();
            is_resuming = false;
        }

        // It is not possible to co_await in `catch` blocks, so I'm using `handle_exception` here
        co_await r.on_file_start(_currently_replayed_segment->seg.segment_id, _currently_replayed_segment->seg.path, is_resuming).handle_exception([&] (std::exception_ptr eptr) {
            queue_logger.debug("[{}] drain(): an error occured in on_file_start(): {}, will delete and skip the file", _ep, eptr);
            
            // Skip this file altogether
            return delete_segment(_currently_replayed_segment->seg.path).then([&] {
                _currently_replayed_segment.reset();
                return make_ready_future<stop_iteration>(stop_iteration::no);
            });
        });

        try {
            timespec last_mod = co_await get_last_file_modification(_currently_replayed_segment->seg.path);
            gc_clock::duration secs_since_file_mod = std::chrono::seconds(last_mod.tv_sec);

            // Use an untriggered abort source
            abort_source as;
            co_await _currently_replayed_segment->read_mutations(_local_db, as, secs_since_file_mod, r, _stats, true);
        } catch (...) {
            queue_logger.debug("[{}] drain(): an error occured in read_mutations(): {}, will delete and skip the file", _ep, std::current_exception());
        }

        co_await delete_segment(_currently_replayed_segment->seg.path);
        _currently_replayed_segment.reset();

        try {
            co_await r.on_file_end();
        } catch (...) {
            queue_logger.debug("[{}] drain(): an error occured in on_file_end(): {}, ignoring", _ep, std::current_exception());
        }
    };

    // Send the segments that we have so far
    while (has_hints_to_replay()) {
        co_await handle_next_file();
    }

    // We have no more hints to replay on the list, but if we flush now
    // we might pick up some
    co_await flush();

    // Send segments that we picked up
    while (has_hints_to_replay()) {
        co_await handle_next_file();
    }
}

future<stop_iteration> queue_store::segment_in_progress::read_mutations(const database& db, abort_source& as, gc_clock::duration secs_since_file_mod, queue_store::reader& r, manager_stats& stats, bool do_drain) {
    stop_iteration should_stop = stop_iteration::no;
    try {
        co_await commitlog::read_log_file(seg.path, FILENAME_PREFIX, service::get_local_streaming_priority(), [&, this] (commitlog::buffer_and_replay_position buf_rp) -> future<> {
            // The lambda is kept alive by commitlog::read_log_file until the call finishes,
            // so it's safe to access captured variables after co_await

            if (!do_drain && should_stop) {
                // If the reader requested a stop, just read through the whole file and stop
                // TODO(later): Maybe we should throw an exception here in order to stop early?
                co_return;
            }

            // If hint replay is paused, wait in a loop until it is resumed or the store is stopped
            while (utils::get_local_injector().enter("hinted_handoff_pause_hint_replay")) {
                co_await sleep(std::chrono::milliseconds(100));
                if (as.abort_requested()) {
                    break;
                }
            }

            auto& buf = buf_rp.buffer;
            auto& rp = buf_rp.position;

            try {
                auto fm_a_s = decode_mutation(db, buf);
                gc_clock::duration gc_grace_sec = fm_a_s.s->gc_grace_seconds();

                if (gc_clock::now().time_since_epoch() - secs_since_file_mod > gc_grace_sec - hints_flush_period) {
                    // The hint is too old - drop it.
                    //
                    // Files are aggregated for at most manager::hints_timer_period therefore the oldest hint there is
                    // (last_modification - manager::hints_timer_period) old.
                } else {
                    should_stop = co_await r.on_mutation(rp, std::move(fm_a_s));
                }
            // TODO(later): add more context to the log messages which follow
            } catch (no_such_column_family& e) {
                queue_logger.debug("read_mutations(): no_such_column_family: {}", e.what());
                ++stats.discarded;
            } catch (no_such_keyspace& e) {
                queue_logger.debug("read_mutations(): no_such_keyspace: {}", e.what());
                ++stats.discarded;
            } catch (no_column_mapping& e) {
                queue_logger.debug("read_mutations(): {} at {}: {}", seg.path, rp, e.what());
                ++stats.discarded;
            } catch (...) {
                queue_logger.debug("read_mutations(): unexpected error in file {} at {}: {}", seg.path, rp, std::current_exception());
                if (!do_drain) {
                    throw;
                }
            }
            read_up_to = std::max(read_up_to, rp.pos);
        });
    } catch (db::commitlog::segment_error& ex) {
        queue_logger.error("{}: {}. Dropping...", seg.path, ex.what());
        ++stats.corrupted_files;
        // Don't propagate the exception, we will consider this file to be done
        // TODO(later): Should we delete the file immediately?
    } catch (...) {
        queue_logger.trace("sending of {} failed: {}", seg.path, std::current_exception());
        if (!do_drain) {
            throw;
        }
    }

    co_return should_stop;
}

frozen_mutation_and_schema queue_store::segment_in_progress::decode_mutation(const database& db, fragmented_temporary_buffer& buf) {
    commitlog_entry_reader hr(buf);
    auto& fm = hr.mutation();
    auto& cm = get_column_mapping(fm, hr);
    auto schema = db.find_schema(fm.column_family_id());

    if (schema->version() != fm.schema_version()) {
        mutation m(schema, fm.decorated_key(*schema));
        converting_mutation_partition_applier v(cm, *schema, m.partition());
        fm.partition().accept(cm, v);
        return {freeze(m), std::move(schema)};
    }
    return {std::move(hr).mutation(), std::move(schema)};
}

const column_mapping& queue_store::segment_in_progress::get_column_mapping(const frozen_mutation& fm, const commitlog_entry_reader& hr) {
    auto cm_it = schema_ver_to_column_mapping.find(fm.schema_version());
    if (cm_it == schema_ver_to_column_mapping.end()) {
        if (!hr.get_column_mapping()) {
            throw no_column_mapping(fm.schema_version());
        }

        queue_logger.debug("new schema version {}", fm.schema_version());
        cm_it = schema_ver_to_column_mapping.emplace(fm.schema_version(), *hr.get_column_mapping()).first;
    }

    return cm_it->second;
}

void queue_store::roll_back_to(db::replay_position rp) {
    const foreign_first_segment_id_comparator cmp_sid{_shard_id};

    if (_currently_replayed_segment.has_value()) {
        // Handle the _currently_replayed_segment
        if (cmp_sid(rp.id, _currently_replayed_segment->seg.segment_id)) {
            // Bring back the current segment to the _segments_to_replay list
            _segments_to_replay.push_front(std::move(_currently_replayed_segment->seg));
            _currently_replayed_segment.reset();
        } else if (cmp_sid(_currently_replayed_segment->seg.segment_id, rp.id)) {
            // Rollback RP is actually later than our actual position
            // Don't do anything
            return;
        } else {
            // Jump to an earlier position in the segment
            _currently_replayed_segment->read_up_to = std::min(_currently_replayed_segment->read_up_to, rp.pos);
            return;
        }
    }

    // If we are here, then !_currently_replayed_segment.has_value()

    // Move some segments to confirm into segments to replay list
    while (!_segments_to_confirm.empty() && cmp_sid(rp.id, std::prev(_segments_to_confirm.end())->first)) {
        auto it = std::prev(_segments_to_confirm.end());
        _segments_to_replay.emplace_front(segment {
            .path = it->second,
            .segment_id = it->first,
        });
        _segments_to_confirm.erase(it);
    }
}

future<> queue_store::confirm_flushed_up_to(db::segment_id_type segment_id_up_to) {
    const foreign_first_segment_id_comparator cmp{_shard_id};
    while (!_segments_to_confirm.empty() && !cmp(segment_id_up_to, _segments_to_confirm.begin()->first)) {
        co_await delete_segment(_segments_to_confirm.begin()->second);
        _flushed_up_to_segment_id = _segments_to_confirm.begin()->first;
        _segments_to_confirm.erase(_segments_to_confirm.begin());
        notify_replay_waiters();
        queue_logger.trace("[{}] confirm_flushed_up_to(): remaining segments to confirm: {}", _ep, _segments_to_confirm.size());
    }
}

future<> queue_store::delete_segment(const sstring& fname) {
    co_await _file_update_mutex.lock_shared();
    auto unlock = defer([this] { _file_update_mutex.unlock_shared(); });

    auto store = co_await _store.get_future();
    co_await store->delete_segments({ std::move(fname) });
}

void queue_store::notify_replay_waiters() noexcept {
    if (has_foreign_segments()) {
        queue_logger.trace("[{}] notify_replay_waiters(): not notifying because there are still some foreign segments to replay", _ep);
        return;
    }

    queue_logger.trace("[{}] notify_replay_waiters(): replay position upper bound was updated to {}", _ep, _flushed_up_to_segment_id);
    while (!_replay_waiters.empty() && _replay_waiters.begin()->first <= _flushed_up_to_segment_id) {
        queue_logger.trace("[{}] notify_replay_waiters(): notifying one ({} <= {})", _ep, _replay_waiters.begin()->first, _flushed_up_to_segment_id);
        auto ptr = _replay_waiters.begin()->second;
        (**ptr).set_value();
        (*ptr) = std::nullopt; // Prevent it from being resolved by abort source subscription
        _replay_waiters.erase(_replay_waiters.begin());
    }
}

void queue_store::dismiss_replay_waiters() noexcept {
    queue_logger.debug("[{}] dismiss_replay_waiters(): dismissing {} replay waiters", _ep, _replay_waiters.size());
    for (auto& p : _replay_waiters) {
        auto ptr = p.second;
        (**ptr).set_exception(std::runtime_error(format("Hints manager for {} is stopping", _ep)));
        (*ptr) = std::nullopt; // Prevent it from being resolved by abort source subscription
    }
    _replay_waiters.clear();
}

bool queue_store::has_foreign_segments() const noexcept {
    auto segment_is_local = [this] (db::segment_id_type id) {
        return db::replay_position(id).shard_id() != _shard_id;
    };

    // If there are foreign segments to replay, the first segment will be foreign.

    if (!_segments_to_confirm.empty()) {
        return segment_is_local(_segments_to_confirm.begin()->first);
    }
    if (_currently_replayed_segment.has_value()) {
        return segment_is_local(_currently_replayed_segment->seg.segment_id);
    }
    if (!_segments_to_replay.empty()) {
        return segment_is_local(_segments_to_replay.front().segment_id);
    }
    return false; // No segments
}

future<> queue_store::wait_until_hints_are_flushed_up_to(abort_source& as, db::replay_position up_to_rp) {
    db::segment_id_type target_segment_id = up_to_rp.id;
    queue_logger.debug("[{}] wait_until_hints_are_replayed_up_to(): entering with target {}", _ep, target_segment_id);
    if (!has_foreign_segments() && target_segment_id <= _flushed_up_to_segment_id) {
        queue_logger.debug("[{}] wait_until_hints_are_replayed_up_to(): hints were already replayed above the point ({} <= {})", _ep, target_segment_id, _flushed_up_to_segment_id);
        return make_ready_future<>();
    }

    if (as.abort_requested()) {
        queue_logger.debug("[{}] wait_until_hints_are_replayed_up_to(): already aborted - stopping", _ep);
        return make_exception_future<>(abort_requested_exception());
    }

    auto ptr = make_lw_shared<std::optional<promise<>>>(promise<>());
    auto it = _replay_waiters.emplace(target_segment_id, ptr);
    auto sub = as.subscribe([this, ptr, it] () noexcept {
        if (!ptr->has_value()) {
            // The promise already was resolved by `notify_replay_waiters` and removed from the map
            return;
        }
        queue_logger.debug("[{}] wait_until_hints_are_replayed_up_to(): abort requested - stopping", _ep);
        _replay_waiters.erase(it);
        (**ptr).set_exception(abort_requested_exception());
    });

    return (**ptr).get_future().finally([this, sub = std::move(sub)] {
        queue_logger.debug("[{}] wait_until_hints_are_replayed_up_to(): returning afther the future was satisfied", _ep);
    });
}

}
}
