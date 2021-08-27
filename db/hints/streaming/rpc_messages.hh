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

#include <cstdint>
#include <optional>
#include "frozen_mutation.hh"
#include "db/commitlog/replay_position.hh"
#include "utils/UUID.hh"

namespace db {
namespace hints {

namespace streaming {

// TODO: Document the protocol

// TODO: Is it a good idea? Protocol negotiation something something?
enum class protocol_version : uint32_t {
    v1 = 1,
};

enum class hints_type : uint8_t {
    // Regular hints
    regular = 0,

    // Materialized view hints
    mv = 1,
};

struct open_request {
    protocol_version version = protocol_version::v1;
    hints_type htype = hints_type::regular;
};

struct open_response {
    // An ID which changes every time the node is restarted.
    // Sender caches the last seen ID. If it notices that the cookie
    // has changed on reconnect, it indicates that the node has been restarted.
    utils::UUID cookie;
};

enum class sender_message_type : uint8_t {
    noop = 0, // Can be used to announce the size of the first mutation
    mutation = 1,
    status_request = 2,
    flush_request = 3,
};

struct sender_message {
    sender_message_type type = sender_message_type::noop;
    uint64_t next_message_memory_reservation = 0;

    // When the original destination for a hint is no longer its replica,
    // we send it to all current replicas. We need to differentiate
    // the original destinations because we use replay positions for tracking
    // progress, and RPs from different hint queues do not mix.
    gms::inet_address original_destination;

    db::replay_position rp;

    // used for: mutation
    std::optional<frozen_mutation> fm;

    std::optional<uint64_t> request_token;
};

enum class receiver_message_type : uint8_t {
    // Contains stats about how many hints were saved to memtables/sstables
    status = 0,
};

struct receiver_message {
    receiver_message_type type = receiver_message_type::status;

    // Which endpoint this confirmation applies to?
    gms::inet_address original_destination;

    // Up to which RP mutations were applied?
    // If no hints were applied _on this connection_ yet, it will be zero
    db::replay_position applied_up_to;

    // Up to which RP mutations were persisted on disk?
    // If no hints were persisted _on this connection_ yet, it will be zero
    db::replay_position flushed_up_to;

    std::optional<uint64_t> response_token;
};

}

}
}
