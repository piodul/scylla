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

#include <set>
#include <unordered_map>
#include <seastar/core/distributed.hh>
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/shared_ptr.hh>
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

#include "seastarx.hh"

namespace service {
class migration_manager;
class storage_proxy;
}

namespace db {
namespace hints {
namespace streaming {

// TODO: Should very old and inactive sessions clean themselves up after a long time?
// TODO: Metrics

class receiver_service final
        : public seastar::async_sharded_service<receiver_service>
        , public seastar::peering_sharded_service<receiver_service>
        , public db::flush_listener {

private:
    // Represents the progress of receiving hints from an endpoint
    // and of particular type (regular vs. MV).
    // There can be at most one RPC stream working on a session.
    // The session state is preserved in case of a disconnect, so that
    // the progress is not lost on temporary network failure.
    struct endpoint_session {
    public:
        // If non-null, there is an active rpc connection operating on this session.
        // The boolean can be used to tell the session to stop itself.
        lw_shared_ptr<bool> stop_flag;

        struct source_state {
            db::replay_position applied_up_to;

            // For each memtable, what is the lowest ID of a mutation included in it?
            std::unordered_map<memtable::id, db::replay_position> memtable_to_lowest_rp;

            // Contains IDs from `memtable_to_lowest_rp`, sorted
            std::set<db::replay_position, foreign_first_rp_comparator> lowest_rps;

            db::replay_position get_flushed_up_to() const {
                if (lowest_rps.empty()) {
                    return applied_up_to;
                }

                db::replay_position rp = *lowest_rps.begin();
                rp.pos--;
                return rp;
            }

            source_state(unsigned shard_id) : lowest_rps(foreign_first_rp_comparator{shard_id}) {}
        };

        std::unordered_map<gms::inet_address, source_state> per_source_state;
    
    private:
        future<> send_status_report(netw::msg_addr from, gms::inet_address source, rpc::sink<receiver_message> sink);

    public:
        inline bool has_active_rpc() const {
            return bool(stop_flag);
        }

        future<> run_rpc_stream_task(
            netw::msg_addr from,
            service::storage_proxy& sp,
            service::migration_manager& mm,
            netw::messaging_service& ms,
            database& db,
            rpc::source<sender_message> source,
            rpc::sink<receiver_message> sink
        );
    };

private:
    // The netw::inet_addr struct _explicitly ignores_ shard ID in comparisions.
    // However, we do want to know the sender's shard because we want to keep
    // session information for each sender. If there were two sender shards
    // connected to one receiver shard, either one would block another because
    // we don't allow two sessions at the same time from the same source, or
    // they will invalidate each other's sessions (because of different cookies).
    std::unordered_map<std::tuple<gms::inet_address, uint32_t, hints_type>, endpoint_session, utils::tuple_hash> _sessions;

    utils::UUID _cookie;

    db::flush_listener_list::handle _flush_listener_registration;
    seastar::gate _rpc_stream_gate;
    seastar::gate _flush_report_gate;

    netw::messaging_service& _ms;

private:
    void register_rpc_verbs(netw::messaging_service& ms, service::migration_manager& mm,
            service::storage_proxy& sp, database& db);
    future<> unregister_rpc_verbs(netw::messaging_service& ms);

    virtual void on_successful_flush(memtable::id id) override;
    virtual void on_failed_flush(memtable::id id) override;

public:
    receiver_service(netw::messaging_service& ms);
    ~receiver_service();

    future<> start(database& db, netw::messaging_service& ms, service::migration_manager& mm,
            service::storage_proxy& sp);
    future<> stop();
};

}
}
}

