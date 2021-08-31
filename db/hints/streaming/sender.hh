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

// TODO: Optimize includes

#include <set>
#include <unordered_map>
#include <seastar/core/distributed.hh>
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/rpc/rpc_types.hh>
#include "db/hints/rp_comparator.hh"
#include "db/hints/streaming/rpc_messages.hh"
#include "db/flush_listener.hh"
#include "message/msg_addr.hh"
#include "memtable.hh"
#include "utils/UUID.hh"
#include "utils/hash.hh"
#include "database_fwd.hh"
#include "message/messaging_service_fwd.hh"
#include "locator/token_metadata.hh"
#include "db/hints/streaming/task_map.hh"

class frozen_mutation;

namespace db {
namespace hints {
namespace streaming {

// TODO: Should we clean very old, inactive sessions?
// TODO: Metrics

struct replay_status {
    uint64_t applied_up_to;
    uint64_t flushed_up_to;
};

class sender_proxy final {
public:
    class rpc_session {
    private:
        std::unordered_map<uint64_t, promise<receiver_message>> _pending_requests;
        uint64_t _next_request_id = 1;

        rpc::sink<sender_message> _sink;
        rpc::source<receiver_message> _source;

        utils::UUID _cookie;

        std::optional<promise<>> _closed;
        shared_future<> _stopped;

    private:
        future<> do_run();
        void close();

    public:
        // Must be public because make_lw_shared doesn't work otherwise
        rpc_session(utils::UUID cookie, rpc::sink<sender_message> sink, rpc::source<receiver_message> source);

        static future<lw_shared_ptr<rpc_session>> start(netw::messaging_service& ms, gms::inet_address addr);

        future<> run();
        future<> stop();

        future<> send_message(const sender_message& msg);
        future<receiver_message> send_request(sender_message& request);
        future<replay_status> query_status();
        future<> request_flush();

        const utils::UUID& get_cookie() const;

        bool is_closed() const;
    };

private:
    // If the connection broke, it will be marked as nullptr
    task_map<gms::inet_address, rpc_session> _sessions;
    netw::messaging_service& _ms;
    bool _stopped = false;

public:
    sender_proxy(netw::messaging_service& ms);
    ~sender_proxy();

    future<> start();
    future<> stop();

    future<lw_shared_ptr<rpc_session>> get_or_create_session(gms::inet_address ep);
};

// TODO: Register for cluster events, i.e. removed nodes
class one_owner_sender {
private:
    struct endpoint_state {
        uint64_t sent_up_to;
        uint64_t applied_up_to;
        uint64_t flushed_up_to;

        utils::UUID cached_cookie;
        lw_shared_ptr<sender_proxy::rpc_session> cached_session;

        endpoint_state(uint64_t rp) {
            sent_up_to = rp;
            applied_up_to = rp;
            flushed_up_to = rp;
        }
    };

private:
    sender_proxy& _proxy;
    gms::inet_address _main_destination;
    std::unordered_map<gms::inet_address, endpoint_state> _ep_states;

    locator::token_metadata_ptr _token_metadata;
    uint64_t _next_mutation_id = 1;

public:
    one_owner_sender(locator::token_metadata_ptr token_metadata, sender_proxy& proxy, gms::inet_address main_destination);

    future<uint64_t> refresh_connections();
    future<uint64_t> send_mutation(database& db, frozen_mutation_and_schema&& fms);
    future<> request_flush();

    // Returns the lowest position up to which we are sure that hints were flushed.
    future<uint64_t> query_status();

    // TODO: Listening for flush events? A structure which accelerates the query for the lowest flushed position?

    uint64_t get_flush_position() const;
};

}
}
}
