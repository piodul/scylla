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

    // An ID which changes every time the node is restarted.
    // Receiver caches this ID. If the stream is broken or closed,
    // receiver will know if the node was restarted or not.
    // Because information about hint progress is not persisted, it is used
    // to identify if unconfirmed hints should be restarted.
    utils::UUID cookie;
};

struct open_response {
    // If true, then the cookie was known by the receiver.
    // If it's not and it is not the first time the session between those
    // two nodes was initiated, it might indicate that the node was restarted,
    // so the sender will be informed that it must re-send unconfirmed hints.
    bool cookie_known;
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

    // used for: mutation
    std::optional<frozen_mutation> fm;
};

enum class receiver_message_type : uint8_t {
    // Contains stats about how many hints were saved to mmtables/sstables
    status = 0,
};

struct receiver_message {
    receiver_message_type type = receiver_message_type::status;

    // How many mutations were applied to memtables?
    uint64_t applied_up_to = 0;

    // What is the largest mutation number that all mutations before it were
    // flushed to disk?
    uint64_t flushed_up_to = 0;
};

}

}
}
