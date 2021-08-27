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
#include <seastar/core/map_reduce.hh>
#include "database.hh"
#include "locator/abstract_replication_strategy.hh"
#include "message/messaging_service.hh"
#include "db/hints/streaming/sender.hh"

namespace db {
namespace hints {
namespace streaming {

class min_rp_reducer {
private:
    const foreign_first_rp_comparator _comparator;
    db::replay_position _result;

public:
    min_rp_reducer(unsigned shard_id) : _comparator(shard_id) {}

    void operator()(db::replay_position rp) {
        if (_result == db::replay_position{} || _comparator(rp, _result)) {
            _result = rp;
        }
    }

    db::replay_position get() const {
        return _result;
    }
};

future<> sender_proxy::rpc_session::run_receiver(rpc::source<receiver_message> source) {
    while (true) {
        auto msg_opt = co_await source();
        if (!msg_opt) {
            break;
        }
        auto& [msg] = *msg_opt;

        // TODO: Verify that the received message type is known
        // Now assume that it's a status report

        if (!msg.response_token.has_value()) {
            // TODO: For now we ignore messages issued by the other side
            continue;
        }

        if (auto it = _pending_requests.find(*msg.response_token); it != _pending_requests.end()) {
            replay_status rs {
                .applied_up_to = msg.applied_up_to,
                .flushed_up_to = msg.flushed_up_to,
            };

            it->second.set_value(rs);
            _pending_requests.erase(it);
        }
    }
}

sender_proxy::rpc_session::rpc_session(rpc::source<receiver_message> source, rpc::sink<sender_message> sink)
        : _sink(std::move(sink)) {

    _finished = run_receiver(std::move(source));
}

future<lw_shared_ptr<sender_proxy::rpc_session>> sender_proxy::rpc_session::open(gms::inet_address ep, hints_type htype, netw::messaging_service& ms) {
    open_request req {
        .version = protocol_version::v1,
        .htype = htype,
    };

    auto [sink, source, resp] = co_await ms.make_sink_and_source_for_hint_stream(netw::msg_addr { ep, 0 }, req);
    co_return make_lw_shared<rpc_session>(std::move(source), std::move(sink));
}

future<> sender_proxy::rpc_session::send_message(const sender_message& msg) {
    return _sink(msg);
}

future<replay_status> sender_proxy::rpc_session::query_status(gms::inet_address original_destination) {
    const uint64_t request_id = _next_request_id++;
    sender_message msg {
        .type = sender_message_type::status_request,
        .original_destination = original_destination,
        .request_token = request_id,
    };

    promise<replay_status> p;
    auto f = p.get_future();
    _pending_requests.insert_or_assign(request_id, std::move(p));

    try {
        co_await _sink(msg);
    } catch (...) {
        _pending_requests.erase(request_id);
        // TODO: What to do with the future f here?
        throw;
    }

    co_return co_await std::move(f);
}

future<> sender_proxy::rpc_session::request_flush() {
    sender_message msg {
        .type = sender_message_type::flush_request,
    };
    return _sink(msg);
}

future<> sender_proxy::rpc_session::close() {
    co_await _sink.close();
    co_await std::move(_finished);

    for (auto& [id, p] : _pending_requests) {
        p.set_exception(rpc::closed_error{});
    }
}

sender_proxy::sender_proxy(hints_type htype, netw::messaging_service& ms)
        : _ms(ms)
        , _htype(htype) {
    // Nothing, for now
}

sender_proxy::~sender_proxy() {
    // Noting, for now
}

future<> sender_proxy::start() {
    // TODO
    co_return;
}

future<> sender_proxy::stop() {
    // TODO
    co_return;
}

future<lw_shared_ptr<sender_proxy::rpc_session>> sender_proxy::get_or_create_session(gms::inet_address ep) {
    if (auto it = _sessions.find(ep); it != _sessions.end()) {
        return make_ready_future<lw_shared_ptr<rpc_session>>(it->second);
    }
    auto [it, inserted] = _pending_sessions.try_emplace(ep);
    if (!inserted) {
        return it->second.get_future();
    }
    it->second = rpc_session::open(ep, _htype, _ms);
    return it->second.get_future().then([this, ep] (auto rpcs) {
        _sessions.insert_or_assign(ep, rpcs);
        return rpcs;
    }).finally([this, ep] {
        _pending_sessions.erase(ep);
    });
}

one_owner_sender::one_owner_sender(locator::token_metadata_ptr token_metadata, sender_proxy& proxy, gms::inet_address main_destination)
        : _proxy(proxy)
        , _main_destination(main_destination)
        , _token_metadata(std::move(token_metadata)) {
    // Nothing, for now
}

future<db::replay_position> one_owner_sender::refresh_connections() {
    return map_reduce(_ep_states.begin(), _ep_states.end(), [this] (auto& p) {
        auto& [ep, state] = p;
        if (state.cached_session) {
            // Connection should be usable
            return make_ready_future<db::replay_position>(state.applied_up_to);
        }

        // The connection broke, so we need to re-create it
        return _proxy.get_or_create_session(ep).then([this, &state = p.second] (lw_shared_ptr<sender_proxy::rpc_session> new_ptr) {
            // If the cookie changed, we must consider all confirmed but not flushed hints lost
            if (state.cached_cookie != new_ptr->get_cookie()) {
                state.cached_cookie = new_ptr->get_cookie();
                state.applied_up_to = state.flushed_up_to;
            }

            // Unconfirmed hints must be considered lost
            state.sent_up_to = state.applied_up_to;
            state.cached_session = std::move(new_ptr);
            return make_ready_future<db::replay_position>(state.applied_up_to);
        });
    }, min_rp_reducer{this_shard_id()});
}

// Calculate endpoints appropriate for this mutation.
// If the original target of the mutation is still a replica, it is the only endpoint returned.
// In other case, all current replicas are returned.
static inet_address_vector_replica_set calculate_endpoints_for_mutation(
        database& db, locator::token_metadata_ptr tm,
        gms::inet_address original_destination, const frozen_mutation_and_schema& fms) {

    const auto& keyspace_name = fms.s->ks_name();
    keyspace& ks = db.find_keyspace(keyspace_name);
    auto& rs = ks.get_replication_strategy();
    auto token = dht::get_token(*fms.s, fms.fm.key());
    inet_address_vector_replica_set natural_endpoints = rs.get_natural_endpoints(std::move(token));

    if (std::find(natural_endpoints.begin(), natural_endpoints.end(), original_destination) != natural_endpoints.end()) {
        // TODO: Should we include more nodes on topology changes? E.g. when the target node is being replaced?
        return {original_destination};
    }

    inet_address_vector_topology_change pending_endpoints = tm->pending_endpoints_for(token, keyspace_name);

    // Remove duplicates from pending_endpoints
    auto it = std::remove_if(pending_endpoints.begin(), pending_endpoints.end(), [&natural_endpoints] (gms::inet_address& p) {
        return std::find(natural_endpoints.begin(), natural_endpoints.end(), p) != natural_endpoints.end();
    });

    // Merge into one vector and return
    std::copy(pending_endpoints.begin(), pending_endpoints.end(), std::back_inserter(natural_endpoints));
    return natural_endpoints;
}

future<> one_owner_sender::send_mutation(database& db, db::replay_position rp, frozen_mutation_and_schema&& fms) {
    const auto eps = calculate_endpoints_for_mutation(db, _token_metadata, _main_destination, fms);

    sender_message msg {
        .type = sender_message_type::mutation,
        .original_destination = _main_destination,
        .rp = rp,
        .fm = std::move(fms.fm),
    };

    // Send the mutation to all applicable endpoints
    // The coroutine will keep the frozen mutation alive
    co_await parallel_for_each(eps, [this, &msg, rp] (gms::inet_address ep) {
        return futurize_invoke([this, ep, rp] () {
            if (auto it = _ep_states.find(ep); it != _ep_states.end()) {
                return make_ready_future<endpoint_state*>(&it->second);
            }

            // Try opening a new connection
            return _proxy.get_or_create_session(ep).then([this, rp, ep] (lw_shared_ptr<sender_proxy::rpc_session> rpcs) {
                // TODO: Does this always work?
                db::replay_position prev_rp = rp;
                --prev_rp.pos;

                // Insert a new session
                endpoint_state& state = _ep_states.insert_or_assign(ep, endpoint_state{prev_rp}).first->second;
                state.cached_cookie = rpcs->get_cookie();
                state.cached_session = std::move(rpcs);
                return make_ready_future<endpoint_state*>(&state);
            });
        }).then([this, &msg, ep, rp] (endpoint_state* state) {
            // TODO: This can be a separate function
            if (!state->cached_session) {
                return make_exception_future<>(std::runtime_error(format("sender session broke for {}", ep)));
            }

            const foreign_first_rp_comparator cmp{this_shard_id()};
            if (cmp(rp, state->sent_up_to)) {
                // We already replayed this mutation there, so skip
                return make_ready_future<>();
            }

            return state->cached_session->send_message(msg).then([this, state, rp] {
                state->sent_up_to = rp;
            }).handle_exception([this, state] (std::exception_ptr eptr) {
                // Clear the connection
                return state->cached_session->close().finally([this, &state] {
                    state->cached_session.release();
                }).then([eptr = std::move(eptr)] {
                    return make_exception_future<>(std::move(eptr));
                });
            });
        });
    });
}

future<> one_owner_sender::request_flush() {
    return parallel_for_each(_ep_states, [this] (auto& p) {
        endpoint_state& state = p.second;
        if (!state.cached_session) {
            return make_exception_future<>(std::runtime_error(format("sender session broke for {}", p.first)));
        }

        return state.cached_session->request_flush().handle_exception([this, &state] (std::exception_ptr eptr) {
            // Clear the connection
            return state.cached_session->close().finally([this, &state] {
                state.cached_session.release();
            }).then([eptr = std::move(eptr)] {
                return make_exception_future<>(std::move(eptr));
            });
        });
    });
}

future<db::replay_position> one_owner_sender::query_status() {
    return map_reduce(_ep_states.begin(), _ep_states.end(), [this] (auto& p) {
        endpoint_state& state = p.second;
        if (!state.cached_session) {
            return make_exception_future<db::replay_position>(std::runtime_error(format("sender session broke for {}", p.first)));
        }

        return state.cached_session->query_status(_main_destination).then([this, &state] (replay_status rs) {
            const foreign_first_rp_comparator cmp{this_shard_id()};
            if (state.applied_up_to == db::replay_position{} || cmp(rs.applied_up_to, state.applied_up_to)) {
                state.applied_up_to = rs.applied_up_to;
            }
            if (state.flushed_up_to == db::replay_position{} || cmp(rs.flushed_up_to, state.flushed_up_to)) {
                state.flushed_up_to = rs.flushed_up_to;
            }
            return state.flushed_up_to;
        }).handle_exception([this, &state] (std::exception_ptr eptr) {
            // Clear the connection
            return state.cached_session->close().finally([this, &state] {
                state.cached_session.release();
            }).then([eptr = std::move(eptr)] {
                return make_exception_future<db::replay_position>(std::move(eptr));
            });
        });
    }, min_rp_reducer{this_shard_id()});
}

db::replay_position one_owner_sender::get_flush_position() const {
    min_rp_reducer reducer{this_shard_id()};
    for (auto& [ep, state] : _ep_states) {
        reducer(state.flushed_up_to);
    }
    return reducer.get();
}

// future<> sender_proxy::send_mutation(database& db, db::replay_position rp, gms::inet_address original_destination, frozen_mutation_and_schema fms) {
//     const auto eps = calculate_endpoints_for_mutation(db, _shared_token_metadata.get(), original_destination, fms);

//     // Send the mutation to all applicable endpoints
//     // The coroutine will keep the frozen mutation alive
//     co_await parallel_for_each(eps, [this, &fms] (gms::inet_address ep) -> future<> {
//         return get_or_create_session(ep).then([&fms] (lw_shared_ptr<rpc_session> rpcs) {
//             return rpcs->send_mutation(fms.fm);
//         });
//     });
// }

}
}
}
