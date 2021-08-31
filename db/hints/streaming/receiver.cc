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
#include "utils/UUID_gen.hh"
#include "db/hints/streaming/receiver.hh"

// TODO: Logging
// TODO: Differentiate MV hints from non-MV hints

static logging::logger hslogger("hints_streaming_receiver");

namespace db {
namespace hints {
namespace streaming {

future<> receiver_service::endpoint_session::send_status_report(netw::msg_addr from) {
    receiver_message resp {
        .type = receiver_message_type::status,
        .applied_up_to = _applied_up_to,
        .flushed_up_to = get_flushed_up_to(),
    };
    hslogger.trace("[{}] Reporting: {} mutations applied, {} flushed", from, resp.applied_up_to, resp.flushed_up_to);
    return _sink(resp);            
}

// TODO: Add function for message type -> message name as string conversion

future<> receiver_service::endpoint_session::run_rpc_stream_task(
    netw::msg_addr from,
    service::storage_proxy& sp,
    service::migration_manager& mm,
    netw::messaging_service& ms,
    database& db,
    rpc::source<sender_message> source
) {
    try {
        while (true) {
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

                    _applied_up_to = msg.mutation_id;

                    if (!_memtable_to_lowest_id.contains(mtbl_id)) {
                        _memtable_to_lowest_id[mtbl_id] = _applied_up_to;
                        _lowest_ids.insert(_applied_up_to);
                    }

                    hslogger.trace("[{}] Put mutation #{} into memtable with ID={}", from, mtbl_id, msg.mutation_id);
                }
                break;
            
            case sender_message_type::status_request:
                hslogger.trace("[{}] Received status_request message", from);
                co_await send_status_report(from);
                break;
            
            case sender_message_type::flush_request:
                // TODO: Implement coalescing flushes
                // TODO: Consider doing this asynchronously
                hslogger.trace("[{}] Received flush_request message", from);
                hslogger.info("[{}] Flushing streaming memtables", from);
                // TODO: We should actually flush on all shards!
                // Moreover, this is prone to be racy. We need to wait until
                // all callbacks issued by the flush_all_streaming_memtables
                // finish (on all shards!) and only then respond.
                co_await db.flush_all_streaming_memtables();
                hslogger.info("[{}] Flushing complete", from);
                {
                    receiver_message resp {
                        .type = receiver_message_type::flush_done,
                    };
                    co_await _sink(resp);
                }
                break;

            default:
                throw std::runtime_error(format("unknown message type: {}", uint8_t(msg.type)));
            }
        }
    } catch (...) {
        hslogger.warn("[{}] Got an exception in the receive loop: {}", from, std::current_exception());
    }

    // TODO: Wait for asynchronous operations here

    if (!is_closed()) {
        _closed->set_value();
        _closed.reset();
    }

    try {
        co_await _sink.close();
    } catch (...) {
        hslogger.warn("[{}] Got an exception when trying to close the sink: {}", from, std::current_exception());
    }

    co_return;
}

future<> receiver_service::endpoint_session::run() {
    _closed.emplace();
    return _closed->get_future();
}

void receiver_service::endpoint_session::close() {
    if (is_closed()) {
        return;
    }

    // Send a close request to the other node
    receiver_message msg {
        .type = receiver_message_type::close_request,
    };
    // Waited indirectly
    // TODO: Error handling
    (void)_sink(msg).forward_to(std::move(*_closed));
    _closed.reset();
}

future<> receiver_service::endpoint_session::stop() {
    close();
    return _stopped.get_future();
}

bool receiver_service::endpoint_session::is_closed() const {
    return _closed.has_value();
}

void receiver_service::endpoint_session::on_successful_flush(memtable::id id) {
    if (auto it = _memtable_to_lowest_id.find(id); it != _memtable_to_lowest_id.end()) {
        auto lowest_hint_id = it->second;
        _memtable_to_lowest_id.erase(it);
        _lowest_ids.erase(lowest_hint_id);
    }
}

receiver_service::endpoint_session::endpoint_session(rpc::sink<receiver_message> sink)
        : _closed(promise<>())
        , _sink(std::move(sink)) {
}

future<lw_shared_ptr<receiver_service::endpoint_session>> receiver_service::endpoint_session::start(
    netw::msg_addr from,
    service::storage_proxy& sp,
    service::migration_manager& mm,
    netw::messaging_service& ms,
    database& db,
    rpc::source<sender_message> source,
    rpc::sink<receiver_message> sink
) {
    auto sess = make_lw_shared<endpoint_session>(std::move(sink));
    sess->_stopped = shared_future<>(sess->run_rpc_stream_task(from, sp, mm, ms, db, source));
    co_return sess;
}

void receiver_service::register_rpc_verbs(netw::messaging_service& ms, service::migration_manager& mm, service::storage_proxy& sp, database& db) {
    ms.register_hint_stream([this, &ms, &mm, &sp, &db] (const rpc::client_info& cinfo, open_request req, rpc::source<sender_message> source)
            -> future<rpc::tuple<open_response, rpc::sink<receiver_message>>> {

        if (req.version != protocol_version::v1) {
            throw std::runtime_error(format("unsupported protocol version: {}", uint32_t(req.version)));
        }

        auto sink = ms.make_sink_for_hint_stream(source);

        const auto from = netw::messaging_service::get_source(cinfo);
        auto t = std::tuple<gms::inet_address, uint32_t>(from.addr, from.cpu_id);

        if (_sessions.get_running(t)) {
            throw std::runtime_error(format("a session for {} is already running", from));
        }

        co_await _sessions.get_or_start(t, from, sp, mm, ms, db, std::move(source), sink);

        hslogger.info("[{}] Accepted connection", from);

        open_response resp {
            .cookie = _cookie,
        };
        co_return rpc::tuple<open_response, rpc::sink<receiver_message>>{resp, sink};
    });
}

future<> receiver_service::unregister_rpc_verbs() {
    return _ms.unregister_hint_stream();
}

void receiver_service::on_successful_flush(memtable::id id) {
    // Waited in receiver_service::stop()
    (void)with_gate(_flush_report_gate, [this, id] {
        // Because of cross-shard hints, we need to check on all shards
        return container().invoke_on_all([id] (receiver_service& rs) {
            rs._sessions.for_each_running([id] (const auto& key, lw_shared_ptr<endpoint_session> sess) {
                sess->on_successful_flush(id);
            });
        });
    });
}

void receiver_service::on_failed_flush(memtable::id id) {
    // TODO: We should invalidate all sessions which had this memtable
    hslogger.error("Failed to flush memtable {}", id);
    assert(false); // TODO: Handle it in a sensible way!!!
}

receiver_service::receiver_service(netw::messaging_service& ms)
        : _cookie(utils::UUID_gen::get_time_UUID())
        , _ms(ms) {
}

receiver_service::~receiver_service() {
    // TODO
}

future<> receiver_service::start(database& db, netw::messaging_service& ms, service::migration_manager& mm,
        service::storage_proxy& sp) {

    register_rpc_verbs(ms, mm, sp, db);
    _flush_listener_registration = db.streaming_flush_listeners().register_listener(this);
    co_return;
}

future<> receiver_service::stop() {
    co_await when_all_succeed(_sessions.stop(), unregister_rpc_verbs()).discard_result();
    _flush_listener_registration = nullptr;
    co_await _flush_report_gate.close();
}

}
}
}
