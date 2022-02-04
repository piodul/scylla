#pragma once

#include <type_traits>
#include "utils/result.hh"

namespace utils {

namespace internal {

// Dummy result satisfying ExceptionContainerResult, for static asserts
template<typename T = void>
using dummy_result = result_with_exception<T, std::exception>;

struct noop_converter {
    template<typename T>
    using wrapped_type = T;

    template<typename T>
    static auto wrap(T&& t) {
        return std::move(t);
    }

    template<typename F, typename... Args>
    static auto invoke(F&& f, Args&&... args) {
        return f(std::forward<Args>(args)...);
    }
};

struct futurizing_converter {
    template<typename T>
    using wrapped_type = typename seastar::futurize<T>::type;

    template<typename T>
    static auto wrap(T&& t) {
        if constexpr (seastar::is_future<T>::value) {
            return t;
        } else {
            return seastar::make_ready_future<T>(std::forward<T>(t));
        }
    }

    template<typename F, typename... Args>
    static auto invoke(F&& f, Args&&... args) {
        return seastar::futurize_invoke(std::forward<F>(f), std::forward<Args>(args)...);
    }
};

// TODO: Should we make a virtual base class for this?
// This would allow using non-generic lambdas in result_catch and result_catch_dots.
template<typename Handle, typename R>
concept ExceptionHandle = ExceptionContainerResult<R> && requires (Handle handle) {
    { handle.into_result() } -> std::same_as<R>;
    { handle.into_future() } -> std::same_as<seastar::future<R>>;
};

template<ExceptionContainerResult R>
struct failed_result_handle {
private:
    R&& _failed_result;

public:
    failed_result_handle(R&& r) : _failed_result(std::move(r)) {}

    // Returns the failed result. Can be used only once as this moves out
    // the failed result. This should be called once during the handler's
    // lifetime, preferably as `return handler.into_failure()`.
    R into_result() {
        return std::move(_failed_result);
    }

    seastar::future<R> into_future() {
        return seastar::make_ready_future<R>(std::move(_failed_result));
    }
};

static_assert(ExceptionHandle<failed_result_handle<dummy_result<>>, dummy_result<>>);

template<ExceptionContainerResult R>
struct exception_ptr_handle {
private:
    std::exception_ptr _eptr;

public:
    exception_ptr_handle(std::exception_ptr&& eptr) : _eptr(std::move(eptr)) {}

    // Throws the exception.
    [[noreturn]] R into_result() {
        std::rethrow_exception(std::move(_eptr));
    }

    seastar::future<R> into_future() {
        return seastar::make_exception_future<R>(std::move(_eptr));
    }
};

// Check that it is a valid handle with some arbitrary ExceptionContainerResult
static_assert(ExceptionHandle<exception_ptr_handle<dummy_result<>>, dummy_result<>>);

template<typename Ex, typename Cb>
struct result_catcher {
private:
    Cb _cb;

public:
    result_catcher(Cb&& cb) : _cb(std::move(cb)) {}

    template<ExceptionContainerResult R, typename Converter, typename Continuation>
    auto handle_exception_from_result(const auto& ex, R& original_result, Continuation&& cont)
            -> typename Converter::template wrapped_type<R> {
        if constexpr (std::is_base_of_v<Ex, std::remove_cvref_t<decltype(ex)>>) {
            if constexpr (std::is_invocable_v<Cb, const Ex&, failed_result_handle<R>>) {
                // Invoke with exception reference and handle
                return Converter::template invoke(_cb, ex, failed_result_handle<R>(std::move(original_result)));
            } else {
                // Simplified interface - invoke with reference to exception only
                static_assert(std::is_invocable_v<Cb, const Ex&>,
                        "The handler function does not have a suitable call operator");
                return Converter::template invoke(_cb, ex);
            }
        } else {
            // Let another handler try it
            return cont();
        }
    }

    template<ExceptionContainerResult R, typename Converter, typename Continuation>
    auto wrap_in_catch(Continuation&& cont) {
        return [this, cont = std::move(cont)] () mutable -> typename Converter::template wrapped_type<R> {
            try {
                return cont();
            } catch (const Ex& ex) {
                if constexpr (std::is_invocable_v<Cb, const Ex&, exception_ptr_handle<R>>) {
                    // Invoke with exception reference and handle
                    return Converter::template invoke(_cb, ex, exception_ptr_handle<R>(std::current_exception()));
                } else {
                    // Simplified interface - invoke with reference to exception only
                    static_assert(std::is_invocable_v<Cb, const Ex&>,
                            "The handler function does not have a suitable call operator");
                    return Converter::template invoke(_cb, ex);
                }
            }
        };
    }
};

template<typename Cb>
struct result_catcher_dots {
private:
    Cb _cb;

public:
    result_catcher_dots(Cb&& cb) : _cb(std::move(cb)) {}

    template<ExceptionContainerResult R, typename Converter, typename Continuation>
    auto handle_exception_from_result(const auto& ex, R& original_result, Continuation&& cont)
            -> typename Converter::template wrapped_type<R> {
        if constexpr (std::is_invocable_v<Cb, failed_result_handle<R>>) {
            // Invoke with handle
            return Converter::template invoke(_cb, failed_result_handle<R>(std::move(original_result)));
        } else {
            // Simplified interface - invoke without arguments
            static_assert(std::is_invocable_v<Cb>,
                    "The handler function does not have a suitable call operator");
            return Converter::template invoke(_cb);
        }
        // Don't propagate to the next handler. The catch (...) is supposed
        // to match on all errors.
    }

    template<ExceptionContainerResult R, typename Converter, typename Continuation>
    auto wrap_in_catch(Continuation&& cont) {
        return [this, cont = std::move(cont)] () mutable -> typename Converter::template wrapped_type<R> {
            try {
                return cont();
            } catch (...) {
                if constexpr (std::is_invocable_v<Cb, exception_ptr_handle<R>>) {
                    // Invoke with handle
                    return Converter::template invoke(_cb, exception_ptr_handle<R>(std::current_exception()));
                } else {
                    // Simplified interface - invoke without arguments
                    static_assert(std::is_invocable_v<Cb>,
                            "The handler function does not have a suitable call operator");
                    return Converter::template invoke(_cb);
                }
            }
        };
    }
};

// Constructs an `invoke_in_try_catch` function which allows to call a callback
// and handle C++ exceptions using a set of handlers given during construction.
//
// The `Converter` is used to appropriately invoke the exception handlers.
template<ExceptionContainerResult R, typename Converter, typename... CatchHandlers>
struct try_catch_chain_impl {};

template<ExceptionContainerResult R, typename Converter, typename FirstCatchHandler, typename... CatchHandlers>
struct try_catch_chain_impl<R, Converter, FirstCatchHandler, CatchHandlers...> {
    static auto invoke_in_try_catch(auto&& cb, FirstCatchHandler& first_handler, CatchHandlers&... catch_handlers) {
        return try_catch_chain_impl<R, Converter, CatchHandlers...>::invoke_in_try_catch(
                first_handler.template wrap_in_catch<R, Converter>(std::move(cb)),
                catch_handlers...);
    }
};

template<ExceptionContainerResult R, typename Converter>
struct try_catch_chain_impl<R, Converter> {
    static auto invoke_in_try_catch(auto&& cb) {
        // Not using an invoker as we always want it to throw
        return cb();
    }
};

// Given a set of handlers, constructs a visitor which can be used to inspect
// a failed result.
//
// The `Converter` is used to appropriately invoke the exception handlers
// and convert the result to the return type if no handlers match on it.
template<ExceptionContainerResult R, typename Converter, typename... ResultHandlers>
struct combined_handler_impl {};

template<ExceptionContainerResult R, typename Converter, typename FirstResultHandler, typename... ResultHandlers>
struct combined_handler_impl<R, Converter, FirstResultHandler, ResultHandlers...>
        : protected combined_handler_impl<R, Converter, ResultHandlers...> {
private:
    using base = combined_handler_impl<R, Converter, ResultHandlers...>;
    FirstResultHandler _first_handler;

public:
    combined_handler_impl(FirstResultHandler&& first_handler, ResultHandlers&&... result_handlers)
            : base(std::move(result_handlers)...)
            , _first_handler(std::move(first_handler))
    { }

    auto handle(const auto& ex, R& original_result) {
        return _first_handler.template handle_exception_from_result<R, Converter>(
                ex, original_result,
                [this, &ex, &original_result] () mutable {
                    return base::handle(ex, original_result);
                });
    }

    auto with_original_result(R& res) {
        return [this, &res] (const auto& ex) {
            return handle(ex, res);
        };
    }
};

template<ExceptionContainerResult R, typename Converter>
struct combined_handler_impl<R, Converter> {
public:
    auto handle(const auto& ex, R& original_result) {
        // No more handlers to try out, just return
        return Converter::template wrap(std::move(original_result));
    }
};

}

/// \brief Allows to handle C++ exceptions and failed results in a unified way.
///
/// When you modify a code path to return a result<> and you encounter
/// a try..catch block, you can use it to migrate the block so that it handles
/// both C++ exceptions and exceptions stored in the result<>.
///
/// \section Example
///
/// Let's say that you have the following try..catch chain:
///
///   try {
///       return a_function_that_may_throw();
///   } catch (const my_exception& ex) {
///       return 123;
///   } catch (...) {
///       throw;
///   }
///
/// You can add support for results in the following way:
///
///   return utils::result_try([&] {
///       return a_function_that_may_throw_or_return_a_failed_result();
///   },  utils::result_catch<my_exception>([&] (const Ex&) -> result<int> {
///       return 123;
///   }), utils::result_catch_dots([&] (auto&& handle) -> result<int> {
///       return handle.into_result();
///   });
///
/// Each `result_catch` handler declares the exception type it handles
/// and accepts a const reference to it as the first argument.
/// The `result_catch_dots` handler is equivalent to `(...)` so it does not
/// declare exception type or accept a reference to it.
///
/// In addition, each `result_catch` and `result_catch_dots` can optionally
/// accept a reference to an exception handle. The exception handle can be used
/// to return/rethrow the exception being currently handled. Depending on
/// whether the handler is invoked for a C++ exception or a failed result<>,
/// it will have a different type and its `into_result()` method will either
/// rethrow the exception or return the failed result, respectively.
///
/// \section Limitations
///
/// The main limitation of result_try is that is is not as flexible with control
/// flow as a try..catch block is. If you have some logic after the try..catch
/// block and some code paths in it block return and some don't, you need to
/// mark it somehow in the return value - for example, you can change result<T>
/// to result<std::optional<T>> and continue if it is std::nullopt, otherwise
/// return.
///
/// The return type of the first argument must be nothrow move constructible.
/// Because this function uses many recursive calls to simulate a multi-catch
/// clause try..catch block and NRVO is not in general guaranteed, there is
/// a risk that some moves are not elided and one of them can throw
/// in the middle of the stack. This exception can be thrown after we already
/// exited the try..catch blocks for some handlers, so not all of them will
/// participate in handling this exception. Because of the inability to properly
/// handle exceptions from failed moves, hence the nothrow-move-constructibility
/// requirement.
///
/// This function does not work with futures and will trigger a static_assert
/// if you use it with a future-returning function. See result_futurize_try
/// for a version which additionally works with exceptional futures.
template<typename... Handlers>
auto result_try(auto&& fun, Handlers&&... handlers) {
    using return_type = std::invoke_result_t<decltype(fun)>;
    static_assert(!seastar::is_future<return_type>::value,
            "result_try does not work with futures, try using result_futurize_try instead");

    using result_type = return_type;
    static_assert(ExceptionContainerResult<return_type>,
            "The main function passed to result_try does not return a result");

    static_assert(std::is_nothrow_move_constructible_v<result_type>,
            "The return type of the main function in result_try must be nothrow move constructible");

    using combined_handler_type = internal::combined_handler_impl<result_type, internal::noop_converter, Handlers...>;
    using try_catch_chain_type = internal::try_catch_chain_impl<result_type, internal::noop_converter, Handlers...>;

    // Invoke `fun` and catch C++ exceptions if any occur
    auto res = try_catch_chain_type::invoke_in_try_catch(std::move(fun), handlers...);
    if (res) {
        return res;
    }

    // No C++ exceptions but the result is a failure - inspect using a visitor
    auto combined_handler = combined_handler_type(std::forward<Handlers>(handlers)...);
    return res.assume_error().accept(combined_handler.with_original_result(res));
}

/// \brief A version of `result_try` which works with futures. It handles
/// C++ exceptions, failed results and exceptional futures returned from `fun`.
///
/// Migration from a try..catch block or f.handle_exception(...) is similar
/// as in the case of `result_try`, with a small number of differences
/// described below.
///
/// The `fun` function and all exception handlers are futurize_invoked,
/// therefore all C++ exceptions are converted to exceptional futures.
///
/// In order to perform `throw`/`make_exception_future<>(std::current_exception())`,
/// you should prefer `handle.into_future()` instead of `handle.into_result()`.
/// Unlike the latter, the former avoids rethrowing the exception and just
/// returns an exceptional future from a captured exception pointer.
template<typename... Handlers>
auto result_futurize_try(auto&& fun, Handlers&&... handlers) {
    // TODO: Optimize so that we don't use futurize_invoke here.
    // Because of futurize_invoke, any exceptions thrown in `fun`
    // will be converted to exceptional future, then thrown again
    // in invoke_try_catch. This is not needed, we could surround
    // the `fun` in the try..catch handlers and then call it.
    auto f = seastar::futurize_invoke(std::move(fun));
    using future_type = decltype(f);
    using result_type = typename future_type::value_type;
    static_assert(ExceptionContainerResult<result_type>,
            "The main function passed to result_futurize_try does not return a result or a future with result");

    static_assert(std::is_nothrow_move_constructible_v<result_type>,
            "The result type returned by the main function passed to result_futurize_try must be nothrow move constructible");

    using combined_handler_type = internal::combined_handler_impl<result_type, internal::futurizing_converter, Handlers...>;
    using try_catch_chain_type = internal::try_catch_chain_impl<result_type, internal::futurizing_converter, Handlers...>;

    return f.then_wrapped([...handlers = std::move(handlers)] (future_type f) mutable -> future_type {
        if (!f.failed()) {
            result_type res = f.get();
            if (res) {
                return seastar::make_ready_future<result_type>(std::move(res));
            }

            // Handle the exception in result
            auto combined_handler = combined_handler_type(std::forward<Handlers>(handlers)...);
            return res.assume_error().accept(combined_handler.with_original_result(res));
        } else {
            // The future has an exception
            // We need to create a try..catch chain from the handlers
            // and rethrow the exception inside it
            return try_catch_chain_type::invoke_in_try_catch(
                    [f = std::move(f)] () mutable { return seastar::make_ready_future<result_type>(f.get()); },
                    handlers...);

            // If none of the handlers caught the exception, it will exit
            // this function and will be converted to an exceptional future
            // by then_wrapped.
        }
    });
}

/// \brief Represents a `catch (const Ex& ex) {}` part in a `result_try` chain.
///
/// The callback must be a generic lambda, and be callable with one
/// of the following sets of arguments:
///
///    (const Ex&)
///    (const Ex&, Handle&&)
///
/// where `Handle` is satisfies the ExceptionHandle<R> concept
/// and `R` is the result return type.
///
/// See the description of `result_try` for more info.
template<typename Ex, typename Cb>
auto result_catch(Cb&& cb) {
    return internal::result_catcher<Ex, Cb>(std::move(cb));
}

/// \brief Represents a `catch (...) {}` part in a `result_try` chain.
///
/// The callback must be a generic lambda, and be callable with one
/// of the following sets of arguments:
///
///    ()
///    (Handle&&)
///
/// where `Handle` is satisfies the ExceptionHandle<R> concept
/// and `R` is the result return type.
///
/// See the description of `result_try` for more info.
template<typename Cb>
auto result_catch_dots(Cb&& cb) {
    return internal::result_catcher_dots<Cb>(std::move(cb));
}

}
