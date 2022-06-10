/*
 * Copyright (C) 2019-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include <seastar/core/semaphore.hh>
#include <seastar/core/shared_ptr.hh>

#include "db/per_partition_rate_limit_info.hh"

class service_permit {
    struct resources {
        std::optional<seastar::semaphore_units<>> units;
        db::allow_per_partition_rate_limit allow_limit = db::allow_per_partition_rate_limit::no;

        resources(seastar::semaphore_units<> units)
                : units(std::move(units))
        {}
    };

    seastar::lw_shared_ptr<resources> _resources;
    service_permit(seastar::semaphore_units<>&& u) : _resources(seastar::make_lw_shared<resources>(std::move(u))) {}
    friend service_permit make_service_permit(seastar::semaphore_units<>&& permit);
    friend service_permit empty_service_permit();
public:
    size_t count() const { return _resources->units ? _resources->units->count() : 0; };
    service_permit with_allow_limit(db::allow_per_partition_rate_limit allow_limit) && { _resources->allow_limit = allow_limit; return std::move(*this); };
    db::allow_per_partition_rate_limit limiting_allowed() const { return _resources->allow_limit; };
};

inline service_permit make_service_permit(seastar::semaphore_units<>&& permit) {
    return service_permit(std::move(permit));
}

inline service_permit empty_service_permit() {
    return make_service_permit(seastar::semaphore_units<>());
}
