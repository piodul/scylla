/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <cstdint>
#include <exception>
#include <type_traits>
#include <seastar/core/future.hh>
#include "rust/cxx/future.hh"

namespace seastar::rs::internal {

template<typename T>
void* make_ready_future(T* data) noexcept {
    if constexpr (!std::is_void_v<T>) {
        void* ret = (void*)new ::seastar::future<T>(::seastar::make_ready_future<T>(std::move(*data)));
        data->~T();
        return ret;
    } else {
        return (void*)new ::seastar::future<>(::seastar::make_ready_future<>());
    }
}

template<typename T>
void* make_exception_future(std::exception_ptr* eptr) noexcept {
    void* ret = (void*)new ::seastar::future<T>(::seastar::make_exception_future<T>(std::move(*eptr)));
    eptr->~exception_ptr();
    return ret;
}

template<typename T>
union future_poll_state {
    future_poll_state() {}
    ~future_poll_state() {}

    T value;
    std::exception_ptr eptr;
};

template<>
union future_poll_state<void> {
    future_poll_state() {}
    ~future_poll_state() {}

    std::exception_ptr eptr;
};

enum class future_poll_discriminant : uint8_t {
    pending = 0,
    value = 1,
    exception = 2,
    moved_out = 3,
};

struct future_poll_base {
    future_poll_discriminant discr;
    uint8_t ref_count;

    bool available() const {
        return discr == future_poll_discriminant::value || discr == future_poll_discriminant::exception;
    }
};

// TODO: "exceptional future ignored" errors
template<typename T>
struct future_poll : public future_poll_base {
    future_poll_state<T> state;

    T get_value() {
        assert(discr == future_poll_discriminant::value);
        auto ret = std::move(state.value);
        state.value.~T();
        discr = future_poll_discriminant::moved_out;
        return ret;
    }
};

template<>
struct future_poll<void> : public future_poll_base {
    future_poll_state<void> state;

    void get_value() {
        assert(discr == future_poll_discriminant::value);
        discr = future_poll_discriminant::moved_out;
    }
};

template<typename T>
struct future_poll_externs {};

template<typename T>
void future_attach_poll_state(::seastar::future<T>* f, future_poll<T>* fp) noexcept {
    (void)std::move(*f).then_wrapped([fp] (::seastar::future<T>&& f_ready) noexcept {
        if (!f_ready.failed()) {
            fp->discr = future_poll_discriminant::value;
            if constexpr (!std::is_void_v<T>) {
                new (&fp->state.value) T(std::move(f_ready.get()));
            }
        } else {
            fp->discr = future_poll_discriminant::exception;
            new (&fp->state.eptr) std::exception_ptr(f_ready.get_exception());
        }

        if (--fp->ref_count == 0) {
            // The other side released their reference,
            // no need to wake them
            future_poll_externs<T>::dispose(fp);
        } else {
            future_poll_externs<T>::wake(fp);
        }
    });
}

} // namespace internal
