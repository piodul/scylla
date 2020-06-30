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
#include <boost/dynamic_bitset.hpp>
#include "schema_fwd.hh"
#include "timestamp.hh"
#include "bytes.hh"
#include <seastar/util/noncopyable_function.hh>

class mutation;

namespace cdc {

using one_kind_column_set = boost::dynamic_bitset<uint64_t>;

class change_processor {
public:
    virtual ~change_processor() {};

    virtual void begin_timestamp(api::timestamp_type ts, bool is_last) = 0;

    // if ck is null, pre/postimage for static row is requested
    virtual void produce_preimage(const clustering_key* ck, const one_kind_column_set& columns_to_include) = 0;
    virtual void produce_postimage(const clustering_key* ck) = 0;

    virtual void process_change(const mutation& m) = 0;
};

bool should_split(const mutation& base_mutation);
void process_changes_with_splitting(const mutation& base_mutation, change_processor& processor,
        bool enable_preimage, bool enable_postimage);
void process_changes_without_splitting(const mutation& base_mutation, change_processor& processor,
        bool enable_preimage, bool enable_postimage);

}
