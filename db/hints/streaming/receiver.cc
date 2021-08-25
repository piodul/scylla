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

#include <exception>

#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/all.hh>
#include "message/messaging_service.hh"
#include "service/migration_manager.hh"
#include "service/storage_proxy.hh"
#include "database.hh"
#include "db/hints/streaming/receiver.hh"

// TODO: Logging
// TODO: Differentiate MV hints from non-MV hints

static logging::logger hslogger("hints_streaming");

namespace db {
namespace hints {
namespace streaming {

future<> receiver_service::endpoint_session::send_status_report(netw::msg_addr from, rpc::sink<receiver_message> sink) {
    receiver_message resp {
        .type = receiver_message_type::status,
        .applied_up_to = applied_mutation_count,
        .flushed_up_to = lowest_ids.empty() ? applied_mutation_count : *lowest_ids.begin(),
    };

    hslogger.trace("[{}] Reporting: {} mutations applied, {} flushed", from, resp.applied_up_to, resp.flushed_up_to);
    return sink(std::move(resp));
}

// TODO: Add function for message type -> message name as string conversion

future<> receiver_service::endpoint_session::run_rpc_stream_task(
    netw::msg_addr from,
    service::storage_proxy& sp,
    service::migration_manager& mm,
    netw::messaging_service& ms,
    database& db,
    rpc::source<sender_message> source,
    rpc::sink<receiver_message> sink
) {
    while (!*stop_flag) {
        auto msg_opt = co_await source();
        if (!msg_opt.has_value()) {
            hslogger.trace("[{}] Reached end of stream", from);
            break;
        }
        auto& [msg] = *msg_opt;

        if (msg.type == sender_message_type::mutation && !msg.fm.has_value()) {
            throw std::runtime_error("mutation data missing for `mutation` message");
        } else if (msg.type != sender_message_type::mutation && msg.fm.has_value()) {
            throw std::runtime_error(format("mutation data present for non-`mutation` message ({})", uint32_t(msg.type)));
        }

        switch (msg.type) {
        case sender_message_type::noop:
            hslogger.trace("[{}] Received noop message", from);
            break; // Do nothing, don't reserve memory yet

        case sender_message_type::mutation:
            {
                hslogger.trace("[{}] Received mutation message, size={}", from, msg.fm->representation().size());

                const schema_ptr s = co_await mm.get_schema_for_write(msg.fm->schema_version(), from, ms);
                const memtable::id mtbl_id = co_await sp.mutate_streaming_mutation(s, *msg.fm);
                const uint64_t hint_id = applied_mutation_count++;

                if (!memtable_to_lowest_id.contains(mtbl_id)) {
                    memtable_to_lowest_id[mtbl_id] = hint_id;
                    lowest_ids.insert(hint_id);
                }

                hslogger.trace("[{}] Put mutation #{} into memtable with ID {}", from, mtbl_id, hint_id);
            }
            break;
        
        case sender_message_type::status_request:
            hslogger.trace("[{}] Received status_request message", from);
            co_await send_status_report(from, sink);
            break;
        
        case sender_message_type::flush_request:
            // TODO: Implement coalescing flushes
            // TODO: Consider doing this asynchronously
            hslogger.trace("[{}] Received flush_request message", from);
            hslogger.info("[{}] Flushing streaming memtables", from);
            co_await db.flush_all_streaming_memtables();
            hslogger.info("[{}] Flushing complete", from);
            break;

        default:
            throw std::runtime_error(format("unknown message type: {}", uint8_t(msg.type)));
        }
    }

    if (*stop_flag) {
        hslogger.info("[{}] Flushing streaming memtables because of shutdown", from);
        co_await db.flush_all_streaming_memtables();
        hslogger.info("[{}] Flushing complete", from);
        co_await send_status_report(from, sink);
    }

    co_return;
}

void receiver_service::register_rpc_verbs(netw::messaging_service& ms, service::migration_manager& mm, service::storage_proxy& sp, database& db) {
    ms.register_hint_stream([this, &ms, &mm, &sp, &db] (const rpc::client_info& cinfo, open_request req, rpc::source<sender_message> source)
            -> future<rpc::tuple<open_response, rpc::sink<receiver_message>>> {

        if (req.version != protocol_version::v1) {
            throw std::runtime_error("unsupported protocol version");
        }

        const auto from = netw::messaging_service::get_source(cinfo);
        auto p = std::pair<gms::inet_address, uint32_t>(from.addr, from.cpu_id);

        bool cookie_known = false;
        auto& sess = [&] () -> endpoint_session& {
            if (auto it = _sessions.find(p); it != _sessions.end()) {
                // We don't allow more than one RPC session for a given endpoint
                if (it->second.has_active_rpc()) {
                    throw std::runtime_error("a session is already present for this endpoint");
                }

                if (it->second.sender_cookie == req.cookie) {
                    cookie_known = true;
                    return it->second;
                }

                // Wrong cookie, we have to invalidate the old session
                _sessions.erase(it);
            }

            return _sessions.try_emplace(p, req.cookie).first->second;
        }();

        auto sink = ms.make_sink_for_hint_stream(source);

        hslogger.info("Connected with {}, cookie={}, cookie_known={}", from, req.cookie, cookie_known);

        // Waited on in receiver_service::stop()
        (void)with_gate(_rpc_stream_gate, [&] () {
            sess.stop_flag = make_lw_shared<bool>(false);
            return sess.run_rpc_stream_task(from, sp, mm, ms, db, std::move(source), sink).then_wrapped([&sess, sink, from] (future<> f) mutable -> future<> {
                if (!f.failed()) {
                    hslogger.info("Connection with {} was closed", from);
                } else {
                    hslogger.warn("Connection with {} was closed due to an error: {}", from, f.get_exception());
                }
                sess.stop_flag = nullptr;
                try {
                    co_await sink.close();
                } catch (...) {
                    hslogger.warn("Failed to close sink to {}: {}", from, std::current_exception());
                }
            }).discard_result();
        });

        open_response resp {
            .cookie_known = cookie_known,
        };
        co_return rpc::tuple<open_response, rpc::sink<receiver_message>>{resp, sink};
    });
}

future<> receiver_service::unregister_rpc_verbs(netw::messaging_service& ms) {
    return ms.unregister_hint_stream();
}

void receiver_service::on_successful_flush(memtable::id id) {
    // Waited in receiver_service::stop()
    (void)with_gate(_flush_report_gate, [this, id] {
        // Because of cross-shard hints, we need to check on all shards
        return container().invoke_on_all([id] (receiver_service& rs) {
            for (auto& [key, sess] : rs._sessions) {
                if (auto it = sess.memtable_to_lowest_id.find(id); it != sess.memtable_to_lowest_id.end()) {
                    uint64_t lowest_id = it->second;
                    sess.memtable_to_lowest_id.erase(it);
                    sess.lowest_ids.erase(lowest_id);
                }
            }
        });
    });
}

void receiver_service::on_failed_flush(memtable::id id) {
    // TODO: We should invalidate all sessions which had this memtable
    hslogger.error("Failed to flush memtable {}", id);
    assert(false); // TODO: Handle it in a sensible way!!!
}

receiver_service::receiver_service(netw::messaging_service& ms)
        : _ms(ms) {
    // Nothing for now
}

receiver_service::~receiver_service() {
    // TODO
}

future<> receiver_service::start(database& db, netw::messaging_service& ms, service::migration_manager& mm,
        service::storage_proxy& sp) {

    register_rpc_verbs(ms, mm, sp, db);
    _flush_listener_registration = db.streaming_flush_listeners().register_listener(this);

    // TODO
    co_return;
}

future<> receiver_service::stop() {
    // TODO:
    // - Prevent more rpc connections from being made
    // - On existing connections, prevent more mutations from being put to memtables
    // - Flush streaming memtables and send status on existing connections for the last time

    // We will probably have to introduce another verb like streaming does
    // which breaks the stream on the other side... I don't like this pattern

    for (auto& [key, sess] : _sessions) {
        if (sess.stop_flag) {
            *sess.stop_flag = true;
        }
    }

    co_await when_all_succeed(
        unregister_rpc_verbs(_ms),
        _rpc_stream_gate.close()
    );

    _flush_listener_registration = nullptr;
    co_await _flush_report_gate.close();

    co_return;
}

}
}
}
