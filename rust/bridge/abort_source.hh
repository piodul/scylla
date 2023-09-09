#pragma once

#include <memory>
#include <seastar/core/abort_source.hh>

namespace ffi {

struct abort_source_subscription {
    void* waker_opt_ptr;
    seastar::optimized_optional<seastar::abort_source::subscription> sub;

    abort_source_subscription(seastar::abort_source& source) noexcept;

    bool abort_requested() const noexcept {
        return !sub || sub->is_linked();
    }

    void link_with_waker(char* waker_opt_ptr) noexcept {
        this->waker_opt_ptr = reinterpret_cast<void*>(waker_opt_ptr);
    }
};

std::unique_ptr<abort_source_subscription> create_subscription(const seastar::abort_source& source) noexcept;

}
