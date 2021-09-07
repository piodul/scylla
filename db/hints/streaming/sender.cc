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
#include <limits>
#include <seastar/core/coroutine.hh>
#include <seastar/core/map_reduce.hh>
#include "database.hh"
#include "locator/abstract_replication_strategy.hh"
#include "message/messaging_service.hh"
#include "db/hints/streaming/sender.hh"
#include "db/hints/streaming/task_map.hh"

static logging::logger hslogger("hints_streaming_sender");

namespace db {
namespace hints {
namespace streaming {

static uint64_t lower_id(uint64_t a, uint64_t b)  {
    return std::min(a, b);
}

sender_proxy::rpc_session::rpc_session(utils::UUID cookie, rpc::sink<sender_message> sink, rpc::source<receiver_message> source)
        : _sink(std::move(sink))
        , _source(std::move(source))
        , _cookie(cookie) {
}

future<lw_shared_ptr<sender_proxy::rpc_session>> sender_proxy::rpc_session::start(netw::messaging_service& ms, gms::inet_address addr) {
    open_request req {
        .version = protocol_version::v1,
    };

    auto [sink, source, resp] = co_await ms.make_sink_and_source_for_hint_stream(netw::msg_addr { addr, 0 }, req);
    co_return make_lw_shared<rpc_session>(resp.cookie, std::move(sink), std::move(source));
}

future<> sender_proxy::rpc_session::run() {
    _stopped = shared_future<>(do_run());
    _closed.emplace();
    return _closed->get_future();
}

future<> sender_proxy::rpc_session::do_run() {
    try {
        while (true) {
            auto msg_opt = co_await _source();
            if (!msg_opt) {
                break;
            }
            auto& [msg] = *msg_opt;

            if (msg.type == receiver_message_type::close_request) {
                close();
                continue;
            }

            if (!msg.response_token.has_value()) {
                // TODO: For now we ignore messages issued by the other side
                continue;
            }

            if (auto it = _pending_requests.find(*msg.response_token); it != _pending_requests.end()) {
                it->second.set_value(std::move(msg));
                _pending_requests.erase(it);
            } else {
                // TODO: Warning about unknown IDs
            }
        }
    } catch (...) {
        // TODO: Log the exception
        // eptr = std::current_exception();
    }

    for (auto& [id, p] : _pending_requests) {
        p.set_exception(rpc::closed_error{});
    }
}

void sender_proxy::rpc_session::close() {
    // Prevent us from sending more
    // The other side will notice that the stream is closed and will close its half
    // Then we will read all messages from source and stop
    if (_closed.has_value()) {
        // TODO: What to do with the exception?
        // Indirectly waited
        (void)_sink.close().handle_exception([] (std::exception_ptr) {}).forward_to(std::move(*_closed));
        _closed.reset();
    }
}

future<> sender_proxy::rpc_session::stop() {
    close();
    return _stopped.get_future();
}

bool sender_proxy::rpc_session::is_closed() const {
    return _closed.has_value();
}

future<> sender_proxy::rpc_session::send_message(const sender_message& msg) {
    try {
        co_return co_await _sink(msg);
    } catch (...) {
        close();
        throw;
    }
}

future<receiver_message> sender_proxy::rpc_session::send_request(sender_message& request) {
    const uint64_t request_id = _next_request_id++;
    request.request_token = request_id;
    auto it = _pending_requests.insert_or_assign(request_id, promise<receiver_message>()).first;

    try {
        co_await send_message(request);
        co_return co_await it->second.get_future();
    } catch (...) {
        _pending_requests.erase(request_id);
        throw;
    }
}

future<replay_status> sender_proxy::rpc_session::query_status() {
    sender_message request {
        .type = sender_message_type::status_request,
    };
    auto response = co_await send_request(request);

    if (response.type != receiver_message_type::status) {
        throw std::runtime_error(format("bad response type for a status request: {}", uint32_t(response.type)));
    }

    co_return replay_status {
        .applied_up_to = response.applied_up_to,
        .flushed_up_to = response.flushed_up_to,
    };
}

future<> sender_proxy::rpc_session::request_flush() {
    sender_message request {
        .type = sender_message_type::flush_request,
    };
    auto response = co_await send_request(request);

    if (response.type != receiver_message_type::flush_done) {
        throw std::runtime_error(format("bad response type for a status request: {}", uint32_t(response.type)));
    }
}

const utils::UUID& sender_proxy::rpc_session::get_cookie() const {
    return _cookie;
}

sender_proxy::sender_proxy(netw::messaging_service& ms)
        : _ms(ms) {
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
    _stopped = true;
    return _sessions.stop();
}

future<lw_shared_ptr<sender_proxy::rpc_session>> sender_proxy::get_or_create_session(gms::inet_address ep) {
    if (_stopped) {
        throw std::runtime_error("hints sender proxy is stopped");
    }
    co_return co_await _sessions.get_or_start(ep, _ms, ep);
}

one_owner_sender::one_owner_sender(locator::token_metadata_ptr token_metadata, sender_proxy& proxy, gms::inet_address main_destination)
        : _proxy(proxy)
        , _main_destination(main_destination)
        , _token_metadata(std::move(token_metadata)) {
    // Nothing, for now
}

future<uint64_t> one_owner_sender::refresh_connections() {
    return map_reduce(_ep_states.begin(), _ep_states.end(), [this] (auto& p) {
        auto& [ep, state] = p;
        if (!state.cached_session->is_closed()) {
            // Connection should be usable
            return make_ready_future<uint64_t>(state.applied_up_to);
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
            return make_ready_future<uint64_t>(state.applied_up_to);
        });
    }, std::numeric_limits<uint64_t>::max(), lower_id);
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

future<uint64_t> one_owner_sender::send_mutation(database& db, frozen_mutation_and_schema&& fms) {
    const auto eps = calculate_endpoints_for_mutation(db, _token_metadata, _main_destination, fms);
    const uint64_t mutation_id = _next_mutation_id++;
    sender_message msg {
        .type = sender_message_type::mutation,
        .mutation_id = mutation_id,
        .fm = std::move(fms.fm),
    };

    // Send the mutation to all applicable endpoints
    // TODO: Consider creating connections/sessions in parallel
    // Mutations can be sent sequentially - we don't wait for confirmation
    // and we don't want to use too much memory by materializing the mutations

    // Get all relevant sessions (in parallel)
    utils::small_vector<std::pair<gms::inet_address, endpoint_state*>, 3> states;
    states.reserve(eps.size());
    co_await parallel_for_each(eps, [this, mutation_id, &states] (gms::inet_address ep) {
        if (auto it = _ep_states.find(ep); it != _ep_states.end()) {
            // Get existing endpoint state
            states.push_back({ep, &it->second});
            return make_ready_future<>();
        }
        // Create an rpc session
        return _proxy.get_or_create_session(ep).then([this, mutation_id, ep, &states] (lw_shared_ptr<sender_proxy::rpc_session> rpcs) {
            // Create the endpoint state
            endpoint_state& state = _ep_states.insert_or_assign(ep, endpoint_state{mutation_id - 1}).first->second;
            state.cached_cookie = rpcs->get_cookie();
            state.cached_session = std::move(rpcs);
            states.push_back({ep, &state});
        });
    });

    // TODO: Explain in more detail
    // We don't want to use too much memory, so we are sending sequentially
    const foreign_first_rp_comparator cmp{this_shard_id()};
    for (auto [ep, state] : states) {
        co_await state->cached_session->send_message(msg);
        state->sent_up_to = mutation_id;
    }

    co_return mutation_id;
}

future<> one_owner_sender::request_flush() {
    return parallel_for_each(_ep_states, [this] (auto& p) {
        return p.second.cached_session->request_flush();
    });
}

future<uint64_t> one_owner_sender::query_status() {
    std::vector<gms::inet_address> fully_flushed_eps;

    auto flushed_up_to_rp = co_await map_reduce(_ep_states.begin(), _ep_states.end(), [this, &fully_flushed_eps] (auto& p) {
        endpoint_state& state = p.second;
        return state.cached_session->query_status().then([this, ep = p.first, &state, &fully_flushed_eps] (replay_status rs) {
            state.applied_up_to = std::min(state.applied_up_to, rs.applied_up_to);
            state.flushed_up_to = std::min(state.flushed_up_to, rs.flushed_up_to);
            return state.flushed_up_to;
        });
    }, std::numeric_limits<uint64_t>::max(), lower_id);

    std::erase_if(_ep_states, [] (auto& p) {
        return p.second.flushed_up_to == p.second.applied_up_to;
    });

    co_return flushed_up_to_rp == std::numeric_limits<uint64_t>::max() ? 0 : flushed_up_to_rp;
}

uint64_t one_owner_sender::get_flush_position() const {
    if (_ep_states.empty()) {
        return 0;
    }

    uint64_t min = std::numeric_limits<uint64_t>::max();
    for (auto& [ep, state] : _ep_states) {
        min = std::min(min, state.flushed_up_to);
    }
    return min;
}

thread_local uint64_t one_owner_sender::_next_mutation_id = 1;

}
}
}
