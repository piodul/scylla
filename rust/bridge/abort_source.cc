#include "abort_source.hh"
#include "cpp_future.hh"

namespace ffi {

abort_source_subscription::abort_source_subscription(seastar::abort_source& source) noexcept
        : waker_opt_ptr(nullptr)
        , sub(source.subscribe([this] () noexcept {
            seastar_rs_rust_future_wake(waker_opt_ptr);
        })) {
}

std::unique_ptr<abort_source_subscription> create_subscription(const seastar::abort_source& source) noexcept {
    return std::make_unique<abort_source_subscription>(const_cast<seastar::abort_source&>(source));
}

}

extern "C" void seastar_rs_cpp_abort_source_request_abort(void* me) {
    reinterpret_cast<seastar::abort_source*>(me)->request_abort();
}
