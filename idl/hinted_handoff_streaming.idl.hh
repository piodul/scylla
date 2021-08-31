/*
 * Copyright 2021-present ScyllaDB
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

namespace db {
namespace hints {
namespace streaming {

enum class protocol_version : uint32_t {
    v1 = 1,
};

struct open_request {
    db::hints::streaming::protocol_version version;
};

struct open_response {
    utils::UUID cookie;
};

enum class sender_message_type : uint8_t {
    noop = 0, // Can be used to announce the size of the first mutation
    mutation = 1,
    status_request = 2,
    flush_request = 3,
};

struct sender_message {
    db::hints::streaming::sender_message_type type;
    uint64_t next_message_memory_reservation;
    uint64_t mutation_id;
    std::optional<frozen_mutation> fm;
    std::optional<uint64_t> request_token;
};

enum class receiver_message_type : uint8_t {
    status = 0,
    flush_done = 1,
    close_request = 2,
};

struct receiver_message {
    db::hints::streaming::receiver_message_type type;
    uint64_t applied_up_to;
    uint64_t flushed_up_to;
    std::optional<uint64_t> response_token;
};

}
}
}
