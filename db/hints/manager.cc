/*
 * Modified by ScyllaDB
 * Copyright (C) 2017-present ScyllaDB
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
#include <seastar/core/future.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/abort_source.hh>
#include <seastar/core/coroutine.hh>
#include <boost/range/adaptors.hpp>
#include "utils/div_ceil.hh"
#include "db/extensions.hh"
#include "service/storage_proxy.hh"
#include "gms/versioned_value.hh"
#include "seastarx.hh"
#include "converting_mutation_partition_applier.hh"
#include "utils/disk-error-handler.hh"
#include "lister.hh"
#include "db/timeout_clock.hh"
#include "service/priority_manager.hh"
#include "database.hh"
#include "service_permit.hh"
#include "utils/directories.hh"
#include "locator/abstract_replication_strategy.hh"
#include "mutation_partition_view.hh"
#include "utils/runtime.hh"
#include "db/hints/rp_comparator.hh"
#include "utils/phased_barrier.hh"

using namespace std::literals::chrono_literals;

namespace db {
namespace hints {

static logging::logger manager_logger("hints_manager");
const std::string manager::FILENAME_PREFIX("HintsLog" + commitlog::descriptor::SEPARATOR);

const std::chrono::seconds manager::hint_file_write_timeout = std::chrono::seconds(2);
const std::chrono::seconds manager::hints_flush_period = std::chrono::seconds(10);

manager::manager(sstring hints_directory, host_filter filter, int64_t max_hint_window_ms, resource_manager& res_manager, distributed<database>& db)
    : _hints_dir(fs::path(hints_directory) / format("{:d}", this_shard_id()))
    , _host_filter(std::move(filter))
    , _local_snitch_ptr(locator::i_endpoint_snitch::get_local_snitch_ptr())
    , _max_hint_window_us(max_hint_window_ms * 1000)
    , _local_db(db.local())
    , _resource_manager(res_manager)
{}

manager::~manager() {
    assert(_ep_managers.empty());
}

void manager::register_metrics(const sstring& group_name) {
    namespace sm = seastar::metrics;

    _metrics.add_group(group_name, {
        sm::make_gauge("size_of_hints_in_progress", _stats.size_of_hints_in_progress,
                        sm::description("Size of hinted mutations that are scheduled to be written.")),

        sm::make_derive("written", _stats.written,
                        sm::description("Number of successfully written hints.")),

        sm::make_derive("errors", _stats.errors,
                        sm::description("Number of errors during hints writes.")),

        sm::make_derive("dropped", _stats.dropped,
                        sm::description("Number of dropped hints.")),

        sm::make_derive("sent", _stats.sent,
                        sm::description("Number of sent hints.")),

        sm::make_derive("discarded", _stats.discarded,
                        sm::description("Number of hints that were discarded during sending (too old, schema changed, etc.).")),

        sm::make_derive("corrupted_files", _stats.corrupted_files,
                        sm::description("Number of hints files that were discarded during sending because the file was corrupted.")),

        sm::make_gauge("pending_drains", 
                        sm::description("Number of tasks waiting in the queue for draining hints"),
                        [this] { return _drain_lock.waiters(); }),

        sm::make_gauge("pending_sends",
                        sm::description("Number of tasks waiting in the queue for sending a hint"),
                        [this] { return _resource_manager.sending_queue_length(); })
    });
}

future<> manager::start(shared_ptr<service::storage_proxy> proxy_ptr, shared_ptr<gms::gossiper> gossiper_ptr) {
    _proxy_anchor = std::move(proxy_ptr);
    _gossiper_anchor = std::move(gossiper_ptr);
    return lister::scan_dir(_hints_dir, { directory_entry_type::directory }, [this] (fs::path datadir, directory_entry de) {
        ep_key_type ep = ep_key_type(de.name);
        if (!check_dc_for(ep)) {
            return make_ready_future<>();
        }
        get_ep_manager(ep); // TODO: Populate segments to replay
        return make_ready_future<>();
    }).then([this] {
        return compute_hints_dir_device_id();
    }).then([this] {
        set_started();
    });
}

future<> manager::stop() {
    manager_logger.info("Asked to stop");

  auto f = make_ready_future<>();

  return f.finally([this] {
    set_stopping();

    return _draining_eps_gate.close().finally([this] {
        return parallel_for_each(_ep_managers, [] (auto& pair) {
            return pair.second.stop();
        }).finally([this] {
            _ep_managers.clear();
            manager_logger.info("Stopped");
        }).discard_result();
    });
  });
}

future<> manager::compute_hints_dir_device_id() {
    return get_device_id(_hints_dir.native()).then([this](dev_t device_id) {
        _hints_dir_device_id = device_id;
    }).handle_exception([this](auto ep) {
        manager_logger.warn("Failed to stat directory {} for device id: {}", _hints_dir.native(), ep);
        return make_exception_future<>(ep);
    });
}

void manager::allow_hints() {
    boost::for_each(_ep_managers, [] (auto& pair) { pair.second.allow_hints(); });
}

void manager::forbid_hints() {
    boost::for_each(_ep_managers, [] (auto& pair) { pair.second.forbid_hints(); });
}

void manager::forbid_hints_for_eps_with_pending_hints() {
    manager_logger.trace("space_watchdog: Going to block hints to: {}", _eps_with_pending_hints);
    boost::for_each(_ep_managers, [this] (auto& pair) {
        end_point_hints_manager& ep_man = pair.second;
        if (has_ep_with_pending_hints(ep_man.end_point_key())) {
            ep_man.forbid_hints();
        } else {
            ep_man.allow_hints();
        }
    });
}

sync_point::shard_rps manager::calculate_current_sync_point(const std::vector<gms::inet_address>& target_hosts) const {
    sync_point::shard_rps rps;
    for (auto addr : target_hosts) {
        auto it = _ep_managers.find(addr);
        if (it != _ep_managers.end()) {
            const end_point_hints_manager& ep_man = it->second;
            rps[ep_man.end_point_key()] = ep_man.last_written_replay_position();
        }
    }
    return rps;
}

future<> manager::wait_for_sync_point(abort_source& as, const sync_point::shard_rps& rps) {
    abort_source local_as;

    auto sub = as.subscribe([&local_as] () noexcept {
        if (!local_as.abort_requested()) {
            local_as.request_abort();
        }
    });

    bool was_aborted = false;
    co_await parallel_for_each(_ep_managers, [this, &was_aborted, &rps, &local_as] (auto& p) {
        const auto addr = p.first;
        auto& ep_man = p.second;

        db::replay_position rp;
        auto it = rps.find(addr);
        if (it != rps.end()) {
            rp = it->second;
        }

        return ep_man.wait_until_hints_are_replayed_up_to(local_as, rp).handle_exception([&local_as, &was_aborted] (auto eptr) {
            if (!local_as.abort_requested()) {
                local_as.request_abort();
            }
            try {
                std::rethrow_exception(std::move(eptr));
            } catch (abort_requested_exception&) {
                was_aborted = true;
            } catch (...) {
                return make_exception_future<>(std::current_exception());
            }
            return make_ready_future();
        });
    });

    if (was_aborted) {
        throw abort_requested_exception();
    }

    co_return;
}

bool manager::end_point_hints_manager::store_hint(schema_ptr s, lw_shared_ptr<const frozen_mutation> fm, tracing::trace_state_ptr tr_state) noexcept {
    return _store.store_hint(s, std::move(fm), std::move(tr_state));
}

// TODO: Is populate_segments_to_replay() really useful?
// Probably yes, it is worth checking if the commitlog was loaded successfully for the first time

// future<> manager::end_point_hints_manager::populate_segments_to_replay() {
//     return with_lock(file_update_mutex(), [this] {
//         return get_or_load().discard_result();
//     });
// }

void manager::end_point_hints_manager::start() {
    clear_stopped();
    allow_hints();
    _store.start();
    _sender.start();
}

future<> manager::end_point_hints_manager::stop(drain should_drain) noexcept {
    if(stopped()) {
        return make_exception_future<>(std::logic_error(format("ep_manager[{}]: stop() is called twice", _key).c_str()));
    }

    return seastar::async([this, should_drain] {
        std::exception_ptr eptr;

        // This is going to prevent further storing of new hints and will break all sending in progress.
        set_stopping();
        _sender.stop(should_drain).handle_exception([&eptr] (auto e) { eptr = std::move(e); }).get();
        _store.stop().handle_exception([&eptr] (auto e) { eptr = std::move(e); }).get();

        if (eptr) {
            manager_logger.error("ep_manager[{}]: exception: {}", _key, eptr);
        }

        set_stopped();
    });
}

manager::end_point_hints_manager::end_point_hints_manager(const key_type& key, manager& shard_manager)
    : _key(key)
    , _shard_manager(shard_manager)
    , _file_update_mutex_ptr(make_lw_shared<seastar::shared_mutex>())
    , _file_update_mutex(*_file_update_mutex_ptr)
    , _store(key, _shard_manager.hints_dir() / format("{}", _key).c_str(), _file_update_mutex, _shard_manager.local_db(), shard_stats())
    , _state(state_set::of<state::stopped>())
    , _sender(*this, _shard_manager.local_storage_proxy(), _shard_manager.local_db(), _shard_manager.local_gossiper())
{}

manager::end_point_hints_manager::~end_point_hints_manager() {
    assert(stopped());
}

manager::end_point_hints_manager& manager::get_ep_manager(ep_key_type ep) {
    auto it = find_ep_manager(ep);
    if (it == ep_managers_end()) {
        manager_logger.trace("Creating an ep_manager for {}", ep);
        manager::end_point_hints_manager& ep_man = _ep_managers.try_emplace(ep, ep, *this).first->second;
        ep_man.start();
        return ep_man;
    }
    return it->second;
}

inline bool manager::have_ep_manager(ep_key_type ep) const noexcept {
    return find_ep_manager(ep) != ep_managers_end();
}

bool manager::store_hint(ep_key_type ep, schema_ptr s, lw_shared_ptr<const frozen_mutation> fm, tracing::trace_state_ptr tr_state) noexcept {
    if (stopping() || draining_all() || !started() || !can_hint_for(ep)) {
        manager_logger.trace("Can't store a hint to {}", ep);
        ++_stats.dropped;
        return false;
    }

    try {
        manager_logger.trace("Going to store a hint to {}", ep);
        tracing::trace(tr_state, "Going to store a hint to {}", ep);

        return get_ep_manager(ep).store_hint(std::move(s), std::move(fm), tr_state);
    } catch (...) {
        manager_logger.trace("Failed to store a hint to {}: {}", ep, std::current_exception());
        tracing::trace(tr_state, "Failed to store a hint to {}: {}", ep, std::current_exception());

        ++_stats.errors;
        return false;
    }
}

bool manager::end_point_hints_manager::sender::can_send() noexcept {
    if (stopping() && !draining()) {
        return false;
    }

    try {
        auto ep_state_ptr = _gossiper. get_endpoint_state_for_endpoint_ptr(end_point_key());
        if (ep_state_ptr && ep_state_ptr->is_alive()) {
            _state.remove(state::ep_state_left_the_ring);
            return true;
        } else {
            if (!_state.contains(state::ep_state_left_the_ring)) {
                _state.set_if<state::ep_state_left_the_ring>(!_shard_manager.local_db().get_token_metadata().is_member(end_point_key()));
            }
            // send the hints out if the destination Node is part of the ring - we will send to all new replicas in this case
            return _state.contains(state::ep_state_left_the_ring);
        }
    } catch (...) {
        return false;
    }
}

bool manager::too_many_in_flight_hints_for(ep_key_type ep) const noexcept {
    // There is no need to check the DC here because if there is an in-flight hint for this end point then this means that
    // its DC has already been checked and found to be ok.
    return _stats.size_of_hints_in_progress > max_size_of_hints_in_progress && !utils::fb_utilities::is_me(ep) && hints_in_progress_for(ep) > 0 && local_gossiper().get_endpoint_downtime(ep) <= _max_hint_window_us;
}

bool manager::can_hint_for(ep_key_type ep) const noexcept {
    if (utils::fb_utilities::is_me(ep)) {
        return false;
    }

    auto it = find_ep_manager(ep);
    if (it != ep_managers_end() && (it->second.stopping() || !it->second.can_hint())) {
        return false;
    }

    // Don't allow more than one in-flight (to the store) hint to a specific destination when the total size of in-flight
    // hints is more than the maximum allowed value.
    //
    // In the worst case there's going to be (_max_size_of_hints_in_progress + N - 1) in-flight hints, where N is the total number Nodes in the cluster.
    if (_stats.size_of_hints_in_progress > max_size_of_hints_in_progress && hints_in_progress_for(ep) > 0) {
        manager_logger.trace("size_of_hints_in_progress {} hints_in_progress_for({}) {}", _stats.size_of_hints_in_progress, ep, hints_in_progress_for(ep));
        return false;
    }

    // check that the destination DC is "hintable"
    if (!check_dc_for(ep)) {
        manager_logger.trace("{}'s DC is not hintable", ep);
        return false;
    }

    // check if the end point has been down for too long
    if (local_gossiper().get_endpoint_downtime(ep) > _max_hint_window_us) {
        manager_logger.trace("{} is down for {}, not hinting", ep, local_gossiper().get_endpoint_downtime(ep));
        return false;
    }

    return true;
}

future<> manager::change_host_filter(host_filter filter) {
    if (!started()) {
        return make_exception_future<>(std::logic_error("change_host_filter: called before the hints_manager was started"));
    }

    return with_gate(_draining_eps_gate, [this, filter = std::move(filter)] () mutable {
        return with_semaphore(drain_lock(), 1, [this, filter = std::move(filter)] () mutable {
            if (draining_all()) {
                return make_exception_future<>(std::logic_error("change_host_filter: cannot change the configuration because hints all hints were drained"));
            }

            manager_logger.debug("change_host_filter: changing from {} to {}", _host_filter, filter);

            // Change the host_filter now and save the old one so that we can
            // roll back in case of failure
            std::swap(_host_filter, filter);

            // Iterate over existing hint directories and see if we can enable an endpoint manager
            // for some of them
            return lister::scan_dir(_hints_dir, { directory_entry_type::directory }, [this] (fs::path datadir, directory_entry de) {
                const ep_key_type ep = ep_key_type(de.name);
                if (_ep_managers.contains(ep) || !_host_filter.can_hint_for(_local_snitch_ptr, ep)) {
                    return make_ready_future<>();
                }
                // return get_ep_manager(ep).populate_segments_to_replay();
                get_ep_manager(ep);
                return make_ready_future<>();
            }).handle_exception([this, filter = std::move(filter)] (auto ep) mutable {
                // Bring back the old filter. The finally() block will cause us to stop
                // the additional ep_hint_managers that we started
                _host_filter = std::move(filter);
            }).finally([this] {
                // Remove endpoint managers which are rejected by the filter
                return parallel_for_each(_ep_managers, [this] (auto& pair) {
                    if (_host_filter.can_hint_for(_local_snitch_ptr, pair.first)) {
                        return make_ready_future<>();
                    }
                    return pair.second.stop(drain::no).finally([this, ep = pair.first] {
                        _ep_managers.erase(ep);
                    });
                });
            });
        });
    });
}

bool manager::check_dc_for(ep_key_type ep) const noexcept {
    try {
        // If target's DC is not a "hintable" DCs - don't hint.
        // If there is an end point manager then DC has already been checked and found to be ok.
        return _host_filter.is_enabled_for_all() || have_ep_manager(ep) ||
               _host_filter.can_hint_for(_local_snitch_ptr, ep);
    } catch (...) {
        // if we failed to check the DC - block this hint
        return false;
    }
}

void manager::drain_for(gms::inet_address endpoint) {
    if (!started() || stopping() || draining_all()) {
        return;
    }

    manager_logger.trace("on_leave_cluster: {} is removed/decommissioned", endpoint);

    // Future is waited on indirectly in `stop()` (via `_draining_eps_gate`).
    (void)with_gate(_draining_eps_gate, [this, endpoint] {
        return with_semaphore(drain_lock(), 1, [this, endpoint] {
            return futurize_invoke([this, endpoint] () {
                if (utils::fb_utilities::is_me(endpoint)) {
                    set_draining_all();
                    return parallel_for_each(_ep_managers, [] (auto& pair) {
                        return pair.second.stop(drain::yes).finally([&pair] {
                            return with_file_update_mutex(pair.second, [&pair] {
                                return remove_file(pair.second.hints_dir().c_str());
                            });
                        });
                    }).finally([this] {
                        _ep_managers.clear();
                    });
                } else {
                    ep_managers_map_type::iterator ep_manager_it = find_ep_manager(endpoint);
                    if (ep_manager_it != ep_managers_end()) {
                        return ep_manager_it->second.stop(drain::yes).finally([this, endpoint, &ep_man = ep_manager_it->second] {
                            return with_file_update_mutex(ep_man, [&ep_man] {
                                return remove_file(ep_man.hints_dir().c_str());
                            }).finally([this, endpoint] {
                                _ep_managers.erase(endpoint);
                            });
                        });
                    }

                    return make_ready_future<>();
                }
            }).handle_exception([endpoint] (auto eptr) {
                manager_logger.error("Exception when draining {}: {}", endpoint, eptr);
            });
        });
    }).finally([endpoint] {
        manager_logger.trace("drain_for: finished draining {}", endpoint);
    });
}

// TODO: Comments
// TODO: Move higher? Or to a separate file?
class legacy_hint_sender final : public queue_store::reader {
private:
    utils::phased_barrier _barrier;
    std::optional<db::segment_id_type> _current_segment_id;
    std::optional<sstring> _segment_name;
    std::unordered_set<db::position_type> _positions_in_progress;

    queue_store& _store;
    manager_stats& _stats;
    gms::inet_address _target_endpoint;
    resource_manager& _resource_manager;
    database& _db;
    service::storage_proxy& _proxy;

    bool _failed = false;

public:
    legacy_hint_sender(queue_store& store, manager_stats& stats, gms::inet_address target_endpoint, resource_manager& rmgr, database& db, service::storage_proxy& proxy)
            : _store(store)
            , _stats(stats)
            , _target_endpoint(target_endpoint)
            , _resource_manager(rmgr)
            , _db(db)
            , _proxy(proxy)
    {}

    future<stop_iteration> on_file_start(db::segment_id_type segment_id, const sstring& file_name, bool is_resuming) noexcept override {
        _current_segment_id = segment_id;
        _segment_name = file_name;
        co_return stop_iteration::no;
    }

    future<stop_iteration> on_file_end() override {
        // Wait for current hints to be sent. We want to know if any of them
        // failed before we move to the next file.
        co_await _barrier.advance_and_await();

        if (_failed) {
            co_return stop_iteration::yes;
        }

        assert(_positions_in_progress.empty());
        assert(_current_segment_id.has_value());

        // Tell the store that we flushed all hints from this file
        // The store will delete current file
        co_await _store.confirm_flushed_up_to(*_current_segment_id);
        manager_logger.trace("send_one_file(): segment {} was sent in full and deleted", *_segment_name);
        _current_segment_id.reset();

        co_return stop_iteration::no;
    }

    future<stop_iteration> on_mutation(db::replay_position rp, frozen_mutation_and_schema fm_a_s) noexcept {
        if (_failed) {
            co_return stop_iteration::yes;
        }

        // TODO: Should we limit memory in the queue_store? We probably should
        auto op = _barrier.start();
        auto units = _resource_manager.get_send_units_for(fm_a_s.fm.representation().size());

        // Waited in stop() via the _send_ops gate
        (void)send_async(rp, std::move(fm_a_s)).finally([op = std::move(op), units = std::move(units)] {});

        co_return stop_iteration::no;
    }

    future<> send_async(db::replay_position rp, frozen_mutation_and_schema fm_a_s) {
        try {
            auto [it_rp, inserted] = _positions_in_progress.insert(rp.pos);
            assert(inserted);

            keyspace& ks = _db.find_keyspace(fm_a_s.s->ks_name());
            auto& rs = ks.get_replication_strategy();
            auto token = dht::get_token(*fm_a_s.s, fm_a_s.fm.key());
            inet_address_vector_replica_set natural_endpoints = rs.get_natural_endpoints(std::move(token));

            if (boost::range::find(natural_endpoints, _target_endpoint) != natural_endpoints.end()) {
                // The fact that we send with CL::ALL in both cases below ensures that new hints are not going
                // to be generated as a result of hints sending.
                manager_logger.trace("Sending directly to {}", _target_endpoint);
                co_await _proxy.send_hint_to_endpoint(std::move(fm_a_s), _target_endpoint);
            } else {
                manager_logger.trace("Endpoints set has changed and {} is no longer a replica. Mutating from scratch...", _target_endpoint);
                co_await _proxy.send_hint_to_all_replicas(std::move(fm_a_s));
            }

            // Remove the position from the set only if the hint was successfully sent
            _positions_in_progress.erase(it_rp);
            ++_stats.sent;
        } catch (...) {
            // This will prevent from sending more hints
            manager_logger.debug("send_one_hint(): failed to send to {}: {}", _target_endpoint, std::current_exception());
            _failed = true;
        }
    }

    future<> stop() {
        return _barrier.advance_and_await();
    };

    std::optional<db::replay_position> get_first_failed_position() const {
        if (_current_segment_id.has_value() && !_positions_in_progress.empty()) {
            return db::replay_position(*_current_segment_id, *std::min_element(_positions_in_progress.begin(), _positions_in_progress.end()));
        }
        return std::nullopt;
    }

    bool has_failed() const {
        return _failed;
    }
};

manager::end_point_hints_manager::sender::sender(end_point_hints_manager& parent, service::storage_proxy& local_storage_proxy, database& local_db, gms::gossiper& local_gossiper) noexcept
    : _stopped(make_ready_future<>())
    , _ep_key(parent.end_point_key())
    , _ep_manager(parent)
    , _shard_manager(_ep_manager._shard_manager)
    , _resource_manager(_shard_manager._resource_manager)
    , _store(_ep_manager._store)
    , _proxy(local_storage_proxy)
    , _db(local_db)
    , _hints_cpu_sched_group(_db.get_streaming_scheduling_group())
    , _gossiper(local_gossiper)
    , _file_update_mutex(_ep_manager.file_update_mutex())
{}

future<> manager::end_point_hints_manager::sender::stop(drain should_drain) noexcept {
    return seastar::async([this, should_drain] {
        set_stopping();
        _stop_as.request_abort();
        _stopped.get();

        // TODO: The interface in the queue_store should be changed
        // so that stop() can drain, too

        if (should_drain == drain::yes) {
            manager_logger.trace("Draining for {}: start", end_point_key());
            legacy_hint_sender sender{_store, shard_stats(), end_point_key(), _resource_manager, _db, _proxy};
            _store.drain(sender).get();
            sender.stop().get();
            manager_logger.trace("Draining for {}: end", end_point_key());
        }
        manager_logger.trace("ep_manager({})::sender: exiting", end_point_key());
    });
}

manager::end_point_hints_manager::sender::clock::duration manager::end_point_hints_manager::sender::next_sleep_duration() const {
    clock::time_point current_time = clock::now();
    clock::time_point next_retry_tp = std::max(_next_send_retry_tp, current_time);

    clock::duration d = next_retry_tp - current_time;

    // Don't sleep for less than 10 ticks of the "clock" if we are planning to sleep at all - the sleep() function is not perfect.
    return clock::duration(10 * div_ceil(d.count(), 10));
}

void manager::end_point_hints_manager::sender::start() {
    seastar::thread_attributes attr;

    attr.sched_group = _hints_cpu_sched_group;
    // TODO: Convert to a real coroutine
    _stopped = seastar::async(std::move(attr), [this] {
        manager_logger.trace("ep_manager({})::sender: started", end_point_key());
        while (!stopping()) {
            try {
                // If there are no hints available for replay at this point - wait.
                _store.wait_for_hints_available_for_replay(_stop_as).get();

                send_hints_maybe();

                // If we got here means that either there are no more hints to send or we failed to send hints we have.
                // In both cases it makes sense to wait a little before continuing.
                sleep_abortable(next_sleep_duration(), _stop_as).get();
            } catch (seastar::sleep_aborted&) {
                break;
            } catch (...) {
                // log and keep on spinning
                manager_logger.trace("sender: got the exception: {}", std::current_exception());
            }
        }
    });
}

// Runs in the seastar::async context
void manager::end_point_hints_manager::sender::send_hints_maybe() noexcept {
    legacy_hint_sender sender{_store, shard_stats(), end_point_key(), _resource_manager, _db, _proxy};

    // TODO: Exception handling?
    _store.read(sender).get();
    sender.stop().get();

    if (sender.has_failed()) {
        auto maybe_pos = sender.get_first_failed_position();
        if (maybe_pos.has_value()) {
            // Roll back to position of the first hint which failed
            _store.roll_back_to(*maybe_pos);
        }
    }
}

static future<> scan_for_hints_dirs(const sstring& hints_directory, std::function<future<> (fs::path dir, directory_entry de, unsigned shard_id)> f) {
    return lister::scan_dir(hints_directory, { directory_entry_type::directory }, [f = std::move(f)] (fs::path dir, directory_entry de) mutable {
        unsigned shard_id;
        try {
            shard_id = std::stoi(de.name.c_str());
        } catch (std::invalid_argument& ex) {
            manager_logger.debug("Ignore invalid directory {}", de.name);
            return make_ready_future<>();
        }
        return f(std::move(dir), std::move(de), shard_id);
    });
}

// runs in seastar::async context
manager::hints_segments_map manager::get_current_hints_segments(const sstring& hints_directory) {
    hints_segments_map current_hints_segments;

    // shards level
    scan_for_hints_dirs(hints_directory, [&current_hints_segments] (fs::path dir, directory_entry de, unsigned shard_id) {
        manager_logger.trace("shard_id = {}", shard_id);
        // IPs level
        return lister::scan_dir(dir / de.name.c_str(), { directory_entry_type::directory }, [&current_hints_segments, shard_id] (fs::path dir, directory_entry de) {
            manager_logger.trace("\tIP: {}", de.name);
            // hints files
            return lister::scan_dir(dir / de.name.c_str(), { directory_entry_type::regular }, [&current_hints_segments, shard_id, ep_addr = de.name] (fs::path dir, directory_entry de) {
                manager_logger.trace("\t\tfile: {}", de.name);
                current_hints_segments[ep_addr][shard_id].emplace_back(dir / de.name.c_str());
                return make_ready_future<>();
            });
        });
    }).get();

    return current_hints_segments;
}

// runs in seastar::async context
void manager::rebalance_segments(const sstring& hints_directory, hints_segments_map& segments_map) {
    // Count how many hints segments to each destination we have.
    std::unordered_map<sstring, size_t> per_ep_hints;
    for (auto& ep_info : segments_map) {
        per_ep_hints[ep_info.first] = boost::accumulate(ep_info.second | boost::adaptors::map_values | boost::adaptors::transformed(std::mem_fn(&std::list<fs::path>::size)), 0);
        manager_logger.trace("{}: total files: {}", ep_info.first, per_ep_hints[ep_info.first]);
    }

    // Create a map of lists of segments that we will move (for each destination end point): if a shard has segments
    // then we will NOT move q = int(N/S) segments out of them, where N is a total number of segments to the current
    // destination and S is a current number of shards.
    std::unordered_map<sstring, std::list<fs::path>> segments_to_move;
    for (auto& [ep, ep_segments] : segments_map) {
        size_t q = per_ep_hints[ep] / smp::count;
        auto& current_segments_to_move = segments_to_move[ep];

        for (auto& [shard_id, shard_segments] : ep_segments) {
            // Move all segments from the shards that are no longer relevant (re-sharding to the lower number of shards)
            if (shard_id >= smp::count) {
                current_segments_to_move.splice(current_segments_to_move.end(), shard_segments);
            } else if (shard_segments.size() > q) {
                current_segments_to_move.splice(current_segments_to_move.end(), shard_segments, std::next(shard_segments.begin(), q), shard_segments.end());
            }
        }
    }

    // Since N (a total number of segments to a specific destination) may be not a multiple of S (a current number of
    // shards) we will distribute files in two passes:
    //    * if N = S * q + r, then
    //       * one pass for segments_per_shard = q
    //       * another one for segments_per_shard = q + 1.
    //
    // This way we will ensure as close to the perfect distribution as possible.
    //
    // Right till this point we haven't moved any segments. However we have created a logical separation of segments
    // into two groups:
    //    * Segments that are not going to be moved: segments in the segments_map.
    //    * Segments that are going to be moved: segments in the segments_to_move.
    //
    // rebalance_segments_for() is going to consume segments from segments_to_move and move them to corresponding
    // lists in the segments_map AND actually move segments to the corresponding shard's sub-directory till the requested
    // segments_per_shard level is reached (see more details in the description of rebalance_segments_for()).
    for (auto& [ep, N] : per_ep_hints) {
        size_t q = N / smp::count;
        size_t r = N - q * smp::count;
        auto& current_segments_to_move = segments_to_move[ep];
        auto& current_segments_map = segments_map[ep];

        if (q) {
            rebalance_segments_for(ep, q, hints_directory, current_segments_map, current_segments_to_move);
        }

        if (r) {
            rebalance_segments_for(ep, q + 1, hints_directory, current_segments_map, current_segments_to_move);
        }
    }
}

// runs in seastar::async context
void manager::rebalance_segments_for(
        const sstring& ep,
        size_t segments_per_shard,
        const sstring& hints_directory,
        hints_ep_segments_map& ep_segments,
        std::list<fs::path>& segments_to_move)
{
    manager_logger.trace("{}: segments_per_shard: {}, total number of segments to move: {}", ep, segments_per_shard, segments_to_move.size());

    // sanity check
    if (segments_to_move.empty() || !segments_per_shard) {
        return;
    }

    for (unsigned i = 0; i < smp::count && !segments_to_move.empty(); ++i) {
        fs::path shard_path_dir(fs::path(hints_directory.c_str()) / seastar::format("{:d}", i).c_str() / ep.c_str());
        std::list<fs::path>& current_shard_segments = ep_segments[i];

        // Make sure that the shard_path_dir exists and if not - create it
        io_check([name = shard_path_dir.c_str()] { return recursive_touch_directory(name); }).get();

        while (current_shard_segments.size() < segments_per_shard && !segments_to_move.empty()) {
            auto seg_path_it = segments_to_move.begin();
            fs::path new_path(shard_path_dir / seg_path_it->filename());

            // Don't move the file to the same location - it's pointless.
            if (*seg_path_it != new_path) {
                manager_logger.trace("going to move: {} -> {}", *seg_path_it, new_path);
                io_check(rename_file, seg_path_it->native(), new_path.native()).get();
            } else {
                manager_logger.trace("skipping: {}", *seg_path_it);
            }
            current_shard_segments.splice(current_shard_segments.end(), segments_to_move, seg_path_it, std::next(seg_path_it));
        }
    }
}

// runs in seastar::async context
void manager::remove_irrelevant_shards_directories(const sstring& hints_directory) {
    // shards level
    scan_for_hints_dirs(hints_directory, [] (fs::path dir, directory_entry de, unsigned shard_id) {
        if (shard_id >= smp::count) {
            // IPs level
            return lister::scan_dir(dir / de.name.c_str(), { directory_entry_type::directory, directory_entry_type::regular }, lister::show_hidden::yes, [] (fs::path dir, directory_entry de) {
                return io_check(remove_file, (dir / de.name.c_str()).native());
            }).then([shard_base_dir = dir, shard_entry = de] {
                return io_check(remove_file, (shard_base_dir / shard_entry.name.c_str()).native());
            });
        }
        return make_ready_future<>();
    }).get();
}

future<> manager::rebalance(sstring hints_directory) {
    return seastar::async([hints_directory = std::move(hints_directory)] {
        // Scan currently present hints segments.
        hints_segments_map current_hints_segments = get_current_hints_segments(hints_directory);

        // Move segments to achieve an even distribution of files among all present shards.
        rebalance_segments(hints_directory, current_hints_segments);

        // Remove the directories of shards that are not present anymore - they should not have any segments by now
        remove_irrelevant_shards_directories(hints_directory);
    });
}

void manager::update_backlog(size_t backlog, size_t max_backlog) {
    if (backlog < max_backlog) {
        allow_hints();
    } else {
        forbid_hints_for_eps_with_pending_hints();
    }
}

class directory_initializer::impl {
    enum class state {
        uninitialized = 0,
        created_and_validated = 1,
        rebalanced = 2,
    };

    utils::directories& _dirs;
    sstring _hints_directory;
    state _state = state::uninitialized;
    seastar::named_semaphore _lock = {1, named_semaphore_exception_factory{"hints directory initialization lock"}};

public:
    impl(utils::directories& dirs, sstring hints_directory)
            : _dirs(dirs)
            , _hints_directory(std::move(hints_directory))
    { }

    future<> ensure_created_and_verified() {
        if (_state > state::uninitialized) {
            return make_ready_future<>();
        }

        return with_semaphore(_lock, 1, [this] () {
            utils::directories::set dir_set;
            dir_set.add_sharded(_hints_directory);
            return _dirs.create_and_verify(std::move(dir_set)).then([this] {
                manager_logger.debug("Creating and validating hint directories: {}", _hints_directory);
                _state = state::created_and_validated;
            });
        });
    }

    future<> ensure_rebalanced() {
        if (_state < state::created_and_validated) {
            return make_exception_future<>(std::logic_error("hints directory needs to be created and validated before rebalancing"));
        }

        if (_state > state::created_and_validated) {
            return make_ready_future<>();
        }

        return with_semaphore(_lock, 1, [this] () {
            manager_logger.debug("Rebalancing hints in {}", _hints_directory);
            return manager::rebalance(_hints_directory).then([this] {
                _state = state::rebalanced;
            });
        });
    }
};

directory_initializer::directory_initializer(std::shared_ptr<directory_initializer::impl> impl)
        : _impl(std::move(impl))
{ }

directory_initializer::~directory_initializer()
{ }

directory_initializer directory_initializer::make_dummy() {
    return directory_initializer{nullptr};
}

future<directory_initializer> directory_initializer::make(utils::directories& dirs, sstring hints_directory) {
    return smp::submit_to(0, [&dirs, hints_directory = std::move(hints_directory)] () mutable {
        auto impl = std::make_shared<directory_initializer::impl>(dirs, std::move(hints_directory));
        return make_ready_future<directory_initializer>(directory_initializer(std::move(impl)));
    });
}

future<> directory_initializer::ensure_created_and_verified() {
    if (!_impl) {
        return make_ready_future<>();
    }
    return smp::submit_to(0, [impl = this->_impl] () mutable {
        return impl->ensure_created_and_verified().then([impl] {});
    });
}

future<> directory_initializer::ensure_rebalanced() {
    if (!_impl) {
        return make_ready_future<>();
    }
    return smp::submit_to(0, [impl = this->_impl] () mutable {
        return impl->ensure_rebalanced().then([impl] {});
    });
}

}
}
