/*
 * Copyright (C) 2022-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <seastar/util/bool_class.hh>

namespace db {

using allow_per_partition_rate_limit = seastar::bool_class<class allow_per_partition_rate_limit_tag>;

}

