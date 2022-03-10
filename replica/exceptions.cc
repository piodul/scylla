/*
 * Copyright 2022-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <concepts>

#include "replica/exceptions.hh"
#include "utils/exceptions.hh"
#include "utils/result_try.hh"


namespace replica {

exception_variant encode_replica_exception(std::exception_ptr eptr) noexcept {
    try {
        try {
            std::rethrow_exception(std::move(eptr));
        } catch (unknown_exception& ex) {
            return std::move(ex);
        } catch (timeout_exception& ex) {
            return std::move(ex);
        } catch (forward_exception& ex) {
            return std::move(ex);
        } catch (virtual_table_update_exception& ex) {
            return std::move(ex);
        } catch (const std::exception& ex) {
            if (is_timeout_exception(ex)) {
                return timeout_exception();
            }
            return unknown_exception(ex.what());
        } catch (...) {
            // TODO: serialize the error message here
            return unknown_exception(seastar::sstring());
        }
    } catch (...) {
        // If an exception happened during construction of the result exception
        // (e.g. failed to allocate an error description string),
        // then a dummy exception will be returned.
        return unknown_exception(seastar::sstring());
    }
}

std::exception_ptr exception_variant::into_exception_ptr() noexcept {
    return std::visit([] <typename Ex> (Ex&& ex) {
        if constexpr (std::is_same_v<Ex, std::monostate>) {
            return std::make_exception_ptr(unknown_exception(seastar::sstring()));
        } else {
            return std::make_exception_ptr(std::move(ex));
        }
    }, std::move(reason));
}

}
