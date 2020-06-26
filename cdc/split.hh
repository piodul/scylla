/*
 * Copyright (C) 2020 ScyllaDB
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

#include <vector>
#include <unordered_set>
#include "schema_fwd.hh"
#include "timestamp.hh"
#include "bytes.hh"
#include <seastar/util/noncopyable_function.hh>

class mutation;

namespace cdc {

using cells_set = std::unordered_set<column_id>;

using preimage_processing_func = seastar::noncopyable_function<void(const dht::decorated_key&, const clustering_key*, std::optional<cells_set>, api::timestamp_type, bytes, int&)>;
using postimage_processing_func = seastar::noncopyable_function<void(const dht::decorated_key&, const clustering_key*, std::optional<cells_set>, api::timestamp_type, bytes, int&)>;
using delta_processing_func = seastar::noncopyable_function<void(mutation, api::timestamp_type, bytes, int&)>;

bool should_split(const mutation& base_mutation, const schema& base_schema);
void for_each_change(const mutation& base_mutation, const schema_ptr& base_schema,
        std::optional<preimage_processing_func> preimage_f,
        std::optional<postimage_processing_func> postimage_f,
        delta_processing_func delta_f);

}
