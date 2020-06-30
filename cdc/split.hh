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

// Represents a set of column ids of one kind (partition key, clustering key, regular row or static row).
// There already exists a column_set type, but it keeps ordinal_column_ids, not column_ids (ordinal column ids
// are unique across whole table, while kind-specific ids are unique only within one column kind).
// To avoid converting back and forth between ordinal and kind-specific ids, one_kind_column_set is used instead.
using one_kind_column_set = boost::dynamic_bitset<uint64_t>;

// An object that processes changes from a single, big mutation.
// It is intended to be used with process_changes_xxx_splitting. Those functions define the order and layout in which
// changes should appear in CDC log, and change_processor is responsible for producing CDC log rows from changes given
// by those two functions.
class change_processor {
protected:
    ~change_processor() {};
public:
    // Tells the processor that changes that follow from now on will be of given timestamp.
    // begin_timestamp can be called only once for a given timestamp and change_processor object.
    //   ts - timestamp of mutation parts
    virtual void begin_timestamp(api::timestamp_type ts) = 0;

    // Tells the processor to produce a preimage for a given clustering/static row.
    //   ck - clustering key of the row for which to produce a preimage; if nullptr, static row preimage is requested
    //   columns_to_include - include information about the current state of those columns only, leave others as null
    virtual void produce_preimage(const clustering_key* ck, const one_kind_column_set& columns_to_include) = 0;

    // Tells the processor to produce a postimage for a given clustering/static row.
    // Contrary to preimage, this requires data from all columns to be present.
    //   ck - clustering key of the row for which to produce a postimage; if nullptr, static row postimage is requested
    virtual void produce_postimage(const clustering_key* ck) = 0;

    // Processes a smaller mutation which is a subset of the big mutation.
    // The mutation provided to process_change should be simple enough for it to be possible to convert it
    // into CDC log rows - for example, it cannot represent a write to two columns of the same row, where
    // both columns have different timestamp or TTL set.
    //   m - the small mutation to be converted into CDC log rows.
    virtual void process_change(const mutation& m) = 0;
};

bool should_split(const mutation& base_mutation);
void process_changes_with_splitting(const mutation& base_mutation, change_processor& processor,
        bool enable_preimage, bool enable_postimage);
void process_changes_without_splitting(const mutation& base_mutation, change_processor& processor,
        bool enable_preimage, bool enable_postimage);

}
