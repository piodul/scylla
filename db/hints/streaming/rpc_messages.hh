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
#include "utils/UUID.hh"

namespace db {
namespace hints {

namespace streaming {

// TODO: Document the protocol

// TODO: Is it a good idea? Protocol negotiation something something?
enum class protocol_version : uint32_t {
    v1 = 1,
};

struct open_request {
    protocol_version version = protocol_version::v1;
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

    // Should not be zero if type == mutation
    uint64_t mutation_id = 0;
    std::optional<frozen_mutation> fm;
    std::optional<uint64_t> request_token;
};

enum class receiver_message_type : uint8_t {
    // Contains stats about how many hints were saved to memtables/sstables
    status = 0,

    // Confirmation that the flush has been done
    flush_done = 1,

    // Request for the other side to close the stream
    close_request = 2,
};

struct receiver_message {
    receiver_message_type type = receiver_message_type::status;

    // Up to which RP mutations were applied?
    // If no hints were applied _on this connection_ yet, it will be zero
    uint64_t applied_up_to;

    // Up to which RP mutations were persisted on disk?
    // If no hints were persisted _on this connection_ yet, it will be zero
    uint64_t flushed_up_to;

    std::optional<uint64_t> response_token;
};

}

}
}
