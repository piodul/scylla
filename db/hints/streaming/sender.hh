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
#include "db/commitlog/replay_position.hh"
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

class frozen_mutation;

namespace db {
namespace hints {
namespace streaming {

// TODO: Should we clean very old, inactive sessions?
// TODO: Metrics
// TODO: loading_shared_values actually can be useful here

struct replay_status {
    db::replay_position applied_up_to;
    db::replay_position flushed_up_to;
};

class sender_proxy final {
public:
    class rpc_session {
    private:
        rpc::sink<sender_message> _sink;
        utils::UUID _cookie;

        std::unordered_map<uint64_t, promise<replay_status>> _pending_requests;
        uint64_t _next_request_id = 1;

        future<> _finished = make_ready_future<>();

    private:
        future<> run_receiver(rpc::source<receiver_message> source);

    public:
        rpc_session(rpc::source<receiver_message> source, rpc::sink<sender_message> sink);

        static future<lw_shared_ptr<rpc_session>> open(gms::inet_address ep, hints_type htype, netw::messaging_service& ms);

        future<> send_message(const sender_message& msg);
        future<replay_status> query_status(gms::inet_address original_destination);
        future<> request_flush();
        future<> close();

        inline bool has_finished() const {
            return _finished.available();
        }

        inline const utils::UUID& get_cookie() const {
            return _cookie;
        }
    };

private:
    // If the connection broke, it will be marked as nullptr
    std::unordered_map<gms::inet_address, lw_shared_ptr<rpc_session>> _sessions;
    std::unordered_map<gms::inet_address, shared_future<lw_shared_ptr<rpc_session>>> _pending_sessions;
    netw::messaging_service& _ms;

    hints_type _htype;

public:
    sender_proxy(hints_type htype, netw::messaging_service& ms);
    ~sender_proxy();

    future<> start();
    future<> stop();

    future<lw_shared_ptr<rpc_session>> get_or_create_session(gms::inet_address ep);
};

// TODO: Register for events 
class one_owner_sender {
private:
    struct endpoint_state {
        db::replay_position sent_up_to;
        db::replay_position applied_up_to;
        db::replay_position flushed_up_to;

        utils::UUID cached_cookie;
        lw_shared_ptr<sender_proxy::rpc_session> cached_session;

        endpoint_state(db::replay_position rp) {
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

public:
    one_owner_sender(locator::token_metadata_ptr token_metadata, sender_proxy& proxy, gms::inet_address main_destination);

    future<db::replay_position> refresh_connections();
    future<> send_mutation(database& db, db::replay_position rp, frozen_mutation_and_schema&& fms);
    future<> request_flush();

    // Returns the lowest position up to which we are sure that hints were flushed.
    future<db::replay_position> query_status();

    // TODO: Listening for flush events? A structure which accelerates the query for the lowest flushed position?

    db::replay_position get_flush_position() const;
};

}
}
}
