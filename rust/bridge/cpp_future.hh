#pragma once

/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// Utilities for passing seastar futures to rust.

#include <memory>
#include <exception>
#include <seastar/core/future.hh>
#include <seastar/core/coroutine.hh>

extern "C" void seastar_rs_rust_future_wake(void* ptr_to_opt_waker) noexcept;

namespace seastar_rs {
namespace internal {

template<typename T>
union future_value_container {
    future_value_container() {}
    ~future_value_container() {}

    T value;
    std::exception_ptr eptr;
};

template<>
union future_value_container<void> {
    future_value_container() {}
    ~future_value_container() {}

    std::exception_ptr eptr;
};

// TODO: Require T to be relocatable
template<typename T>
struct future_tracker {
    void* waker_ptr;
    char ref_count;
    char state; // 0 - nothing, 1 - value, 2 - exception
    future_value_container<T> value;

    future_tracker(void* waker_ptr)
            : waker_ptr(waker_ptr)
            , ref_count(2)
            , state(0)
    {}

    ~future_tracker() {
        if (state == 1) {
            if constexpr (!std::is_void_v<T>) {
                value.value.~T();
            }
        } else if (state == 2) {
            value.eptr.~exception_ptr();
        }
    }

    void on_future_completed(seastar::future<T>&& f) noexcept {
        if (ref_count == 1) {
            // The Rust side is not waiting anymore
            delete this;
            return;
        }
        seastar_rs_rust_future_wake(waker_ptr);
        ref_count = 1;
        if (!f.failed()) {
            state = 1;
            if constexpr (!std::is_void_v<T>) {
                new (&value.value) T(f.get());
            }
        } else {
            state = 2;
            new (&value.eptr) std::exception_ptr(f.get_exception());
        }
    }

    void unref() noexcept {
        if (ref_count == 1) {
            delete this;
        } else {
            ref_count = 1;
        }
    }
};

}
}

// Definition of the future type. Basically, a glorified std::unique_ptr that wraps
// a future.
#define SEASTAR_RS_INTERNAL_DEFINE_FUTURE_TYPE(future_name, passed_type)               \
    struct future_name : public std::unique_ptr<seastar::future<passed_type>> {        \
    public:                                                                            \
        future_name(seastar::future<passed_type> f)                                    \
                : unique_ptr(std::make_unique<seastar::future<passed_type>>(           \
                        std::move(f)))                                                 \
        {}                                                                             \
    };                                                                                 \

#define SEASTAR_RS_INTERNAL_DEFINE_PROMISE_TYPE(promise_name, passed_type)             \
    struct promise_name : public std::unique_ptr<seastar::promise<passed_type>> {      \
    public:                                                                            \
        promise_name(seastar::promise<passed_type> p)                                  \
                : unique_ptr(std::make_unique<seastar::promise<passed_type>>(          \
                        std::move(p)))                                                 \
        {}                                                                             \
    };                                                                                 \

// Coroutine traits for the future type. It should be possible to write coroutines
// that return the boxed future in the same way as regular seastar::future coroutines.
#define SEASTAR_RS_INTERNAL_DEFINE_FUTURE_COROUTINE_TRAITS(future_name, passed_type)   \
    template<typename... Args>                                                         \
    struct ::std::coroutine_traits<future_name, Args...>                               \
            : public ::std::coroutine_traits<seastar::future<passed_type>, Args...> {  \
    public:                                                                            \
        future_name get_return_object() noexcept {                                     \
            return future_name(                                                        \
                    ::std::coroutine_traits<seastar::future<passed_type>, Args...>     \
                            ::get_return_object());                                    \
        }                                                                              \
    };                                                                                 \

// Implementations of the C++-side shims.
// Only meant to be generated in a .cc file, not the headers.
#if defined(SEASTAR_RS_GENERATE_IMPLS)
#define SEASTAR_RS_INTERNAL_DEFINE_FUTURE_CPP_SHIMS(future_name, passed_type)          \
    /* Allocates a future_tracker and attaches the current future to it.               \
       Consumes the original future in the process. */                                 \
    extern "C"                                                                         \
    ::seastar_rs::internal::future_tracker<passed_type>*                               \
    seastar_rs_cpp_shim_##future_name##_allocate_tracker(                              \
            seastar::future<passed_type>* fut_ptr,                                     \
            void* waker_ptr) noexcept {                                                \
        auto* tracker =                                                                \
                new ::seastar_rs::internal::future_tracker<passed_type>(waker_ptr);    \
        (void)fut_ptr->then_wrapped(                                                   \
                [tracker] (::seastar::future<passed_type>&& f) mutable {               \
                    tracker->on_future_completed(std::move(f));                        \
                });                                                                    \
        delete fut_ptr;                                                                \
        return tracker;                                                                \
    }                                                                                  \
                                                                                       \
    extern "C"                                                                         \
    void                                                                               \
    seastar_rs_cpp_shim_##future_name##_free_tracker(                                  \
            ::seastar_rs::internal::future_tracker<passed_type>* tracker) noexcept {   \
        tracker->unref();                                                              \
    }                                                                                  \
                                                                                       \
    extern "C"                                                                         \
    void                                                                               \
    seastar_rs_cpp_shim_##future_name##_drop_future(                                   \
            ::seastar::future<passed_type>* fut_ptr) noexcept {                        \
        delete fut_ptr;                                                                \
    }                                                                                  \

#else
#define SEASTAR_RS_INTERNAL_DEFINE_FUTURE_CPP_SHIMS(future_name, passed_type)
#endif // defined(SEASTAR_RS_GENERATE_IMPLS)

#if defined(SEASTAR_RS_GENERATE_IMPLS)
// TODO: Setting exceptions
#define SEASTAR_RS_INTERNAL_DEFINE_PROMISE_CPP_SHIMS(promise_name, passed_type)        \
    extern "C"                                                                         \
    ::seastar::promise<promise_name>*                                                  \
    seastar_rs_cpp_shim_##promise_name##_new() noexcept {                              \
        new :::seastar::promise<passed_type>();                                        \
    }                                                                                  \
                                                                                       \
    extern "C"                                                                         \
    void                                                                               \
    seastar_rs_cpp_shim_##promise_name##_set_value(                                    \
            ::seastar::promise<passed_type>* p, passed_type* v) {                      \
        if constexpr (std::is_same_v<passed_type, void>) noexcept {                    \
            (void)v;                                                                   \
            p->set_value();                                                            \
        } else {                                                                       \
            p->set_value(std::move(v));                                                \
        }                                                                              \
    }                                                                                  \
                                                                                       \
    extern "C"                                                                         \
    void                                                                               \
    seastar_rs_cpp_shim_##promise_name##_drop_promise(                                 \
            ::seastar::promise<passed_type>* prom_ptr) noexcept {                      \
        delete prom_ptr;                                                               \
    }                                                                                  \

#else
#define SEASTAR_RS_INTERNAL_DEFINE_PROMISE_CPP_SHIMS(promise_name, passed_type)
#endif // defined(SEASTAR_RS_GENERATE_IMPLS)

// This is the macro that should be used from the outside
#define SEASTAR_RS_DEFINE_CPP_FUTURE(future_name, passed_type)                         \
    SEASTAR_RS_INTERNAL_DEFINE_FUTURE_TYPE(future_name, passed_type)                   \
    SEASTAR_RS_INTERNAL_DEFINE_FUTURE_COROUTINE_TRAITS(future_name, passed_type)       \
    SEASTAR_RS_INTERNAL_DEFINE_FUTURE_CPP_SHIMS(future_name, passed_type)              \

#define SEASTAR_RS_DEFINE_CPP_PROMISE(promise_name, passed_type)                       \
    SEASTAR_RS_INTERNAL_DEFINE_PROMISE_TYPE(promise_name, passed_type)                 \
    SEASTAR_RS_INTERNAL_DEFINE_PROMISE_CPP_SHIMS(promise_name, passed_type)            \

// Forward declarations
namespace direct_failure_detector {
class subscription;
}

// Add the future instantiations that you need below this line.
// SEASTAR_RS_DEFINE_CPP_FUTURE(future_void, void);
SEASTAR_RS_DEFINE_CPP_FUTURE(future_bool, bool);
SEASTAR_RS_DEFINE_CPP_FUTURE(future_u32, uint32_t);
// SEASTAR_RS_DEFINE_CPP_FUTURE(future_direct_failure_detector_subscription, std::unique_ptr<direct_failure_detector::subscription>);

SEASTAR_RS_DEFINE_CPP_PROMISE(promise_bool, bool);
SEASTAR_RS_DEFINE_CPP_PROMISE(promise_u32, uint32_t);
