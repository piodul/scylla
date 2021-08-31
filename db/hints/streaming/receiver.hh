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
#include <seastar/core/shared_future.hh>
#include <seastar/rpc/rpc_types.hh>
#include "db/hints/rp_comparator.hh"
#include "db/hints/streaming/rpc_messages.hh"
#include "db/hints/streaming/task_map.hh"
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
    private:
        std::optional<promise<>> _closed;
        shared_future<> _stopped;

        uint64_t _applied_up_to = 0;
        // For each memtable, what is the lowest ID of a mutation included in it?
        std::unordered_map<memtable::id, uint64_t> _memtable_to_lowest_id;
        std::set<uint64_t> _lowest_ids;

        rpc::sink<receiver_message> _sink;

    private:
        future<> send_status_report(netw::msg_addr from);

        inline uint64_t get_flushed_up_to() const {
            return _lowest_ids.empty() ? _applied_up_to : (*_lowest_ids.begin() - 1);
        }

        future<> run_rpc_stream_task(
            netw::msg_addr from,
            service::storage_proxy& sp,
            service::migration_manager& mm,
            netw::messaging_service& ms,
            database& db,
            rpc::source<sender_message> source
        );

        void close();

    public:
        // Must be public so that lw_shared_ptr works
        endpoint_session(rpc::sink<receiver_message> sink);

        static future<lw_shared_ptr<endpoint_session>> start(
            netw::msg_addr from,
            service::storage_proxy& sp,
            service::migration_manager& mm,
            netw::messaging_service& ms,
            database& db,
            rpc::source<sender_message> source,
            rpc::sink<receiver_message> sink
        );

        future<> run();
        future<> stop();

        void on_successful_flush(memtable::id id);

        bool is_closed() const;
    };

private:
    // The netw::inet_addr struct _explicitly ignores_ shard ID in comparisions.
    // However, we do want to know the sender's shard because we want to keep
    // session information for each sender. If there were two sender shards
    // connected to one receiver shard, either one would block another because
    // we don't allow two sessions at the same time from the same source, or
    // they will invalidate each other's sessions (because of different cookies).
    task_map<std::tuple<gms::inet_address, uint32_t>, endpoint_session, utils::tuple_hash> _sessions;

    utils::UUID _cookie;

    db::flush_listener_list::handle _flush_listener_registration;
    seastar::gate _rpc_stream_gate;
    seastar::gate _flush_report_gate;

    netw::messaging_service& _ms;

private:
    void register_rpc_verbs(netw::messaging_service& ms, service::migration_manager& mm,
            service::storage_proxy& sp, database& db);
    future<> unregister_rpc_verbs();

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

