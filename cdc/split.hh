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
#include "schema_fwd.hh"
#include "timestamp.hh"
#include "bytes.hh"
#include <seastar/util/noncopyable_function.hh>

class mutation;

namespace cdc {

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
    //   tuuid - corresponds to the cdc$time
    virtual void begin_timestamp(api::timestamp_type ts, bytes tuuid) = 0;

    // Processes a smaller mutation which is a subset of the big mutation.
    // The mutation provided to process_change should be simple enough for it to be possible to convert it
    // into CDC log rows - for example, it cannot represent a write to two columns of the same row, where
    // both columns have different timestamp or TTL set.
    //   m - the small mutation to be converted into CDC log rows.
    //   batch_no - reference to the cdc$batch_seq_no counter.
    virtual void process_change(const mutation& m, int& batch_no) = 0;
};

bool should_split(const mutation& base_mutation);
void process_changes_with_splitting(const mutation& base_mutation, change_processor& processor);
void process_changes_without_splitting(const mutation& base_mutation, change_processor& processor);

}
