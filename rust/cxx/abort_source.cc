/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <memory>
#include <seastar/core/abort_source.hh>

template<typename Return, typename... Args>
struct boxed_fn_once_closure {
public:
    using caller_fn = Return(*)(void*, Args...);
    using dropper_fn = Return(*)(void*);

private:
    std::unique_ptr<void, dropper_fn> _fn;
    caller_fn _caller;

public:
    boxed_fn_once_closure(void* fn, caller_fn caller, dropper_fn dropper)
            : _fn(fn, dropper)
            , _caller(caller)
    {}

    Return operator()(Args... args) noexcept {
        // FnOnce consumes the object, so we musn't destroy it
        // `fn` must never throw
        auto fn = _fn.release();
        return _caller(fn, args...);
    }
};

extern "C" void seastar_rs_abort_source_subscribe(
    seastar::abort_source* as,
    void* payload,
    void (*caller)(void*, const std::exception_ptr*),
    void (*dropper)(void*),
    std::unique_ptr<seastar::abort_source::subscription>* out_unique_ptr
) noexcept {
    auto subscription_fn = boxed_fn_once_closure(payload, caller, dropper);
    auto sub = as->subscribe([subscription_fn = std::move(subscription_fn)] (const std::optional<std::exception_ptr>& eptr) mutable noexcept {
        const std::exception_ptr* peptr = eptr ? &*eptr : nullptr;
        subscription_fn(peptr);
    });
    if (sub) {
        new (out_unique_ptr) std::unique_ptr<seastar::abort_source::subscription>(
            std::make_unique<seastar::abort_source::subscription>(std::move(*sub))
        );
    } else {
        new (out_unique_ptr) std::unique_ptr<seastar::abort_source::subscription>();
    }
}

extern "C" void seastar_rs_abort_source_request_abort(
    seastar::abort_source* as
) noexcept {
    as->request_abort();
}

extern "C" void seastar_rs_abort_source_request_abort_with_exception(
    seastar::abort_source* as,
    std::exception_ptr* in_eptr
) noexcept {
    as->request_abort_ex(std::move(*in_eptr));
    in_eptr->~exception_ptr();
}

extern "C" int seastar_rs_abort_source_abort_requested(
    seastar::abort_source* as
) noexcept {
    return as->abort_requested() ? 1 : 0;
}

extern "C" int seastar_rs_abort_source_check(
    seastar::abort_source* as,
    std::exception_ptr* out_eptr
) noexcept {
    // There is no way to obtain the exception without throwing, for now
    try {
        as->check();
        return 0;
    } catch (...) {
        new (out_eptr) std::exception_ptr(std::current_exception());
        return 1;
    }
}
