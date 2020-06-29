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

using columns_set = std::unordered_set<column_id>;

class change_processor {
public:
    virtual ~change_processor() {};

    virtual void begin_timestamp(api::timestamp_type ts) = 0;

    // ck is null if static row pre/postimage is requested
    virtual void produce_preimage(const clustering_key* ck, const columns_set& columns_to_include) = 0;
    virtual void process_delta(mutation m) = 0;
    virtual void produce_postimage(const clustering_key* ck) = 0;
};

bool should_split(const mutation& base_mutation, const schema& base_schema);
void process_changes_with_splitting(const mutation& base_mutation, const schema_ptr& base_schema, change_processor& processor,
        bool enable_preimage, bool enable_postimage);
void process_changes_without_splitting(const mutation& base_mutation, const schema_ptr& base_schema, change_processor& processor,
        bool enable_preimage, bool enable_postimage);

}
