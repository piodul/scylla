#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright (C) 2023-present ScyllaDB
#

#
# SPDX-License-Identifier: AGPL-3.0-or-later
#

# A codegen tool that is meant to complement cxx's shortcomings.

# TODO: Document how it works


import os
import sys
import textwrap
import yaml


CURRENT_YEAR = 2023


def get_seastar_crate_path(y):
    if "internal" in y and y["internal"] is True:
        return "crate"
    else:
        return "::seastar"


def escape_cpp_type(cpp: str):
    out = []
    for c in cpp:
        if c.isalnum() or c == '_':
            out.append(c)
        elif c == ':':
            out.append('$')
        else:
            raise Exception(f"Cannot translate character '{c}'")
    return "".join(out)


def fix_name_for_rust(rust: str):
    if rust == "()":
        return "Unit"
    allgood = all(c.isalnum() or c == '_' for c in rust)
    if not allgood:
        raise Exception(f"Cannot adjust rust name to a valid identifier: '{rust}'")
    return "".join(w[0].upper() + w[1:] for w in rust.split('_') if len(w) != 0)


def license():
    # The syntax works both for Rust and C++
    print(textwrap.dedent(f'''\
        /*
        * Copyright (C) {CURRENT_YEAR}-present ScyllaDB
        */

        /*
        * SPDX-License-Identifier: AGPL-3.0-or-later
        */
        '''))


def generate_empty_header(y):
    license()
    print(textwrap.dedent("""\
        #pragma once

        // Empty!
        """))


def generate_rust_futures_promises(y):
    license()
    crate = get_seastar_crate_path(y)
    for item in y["items"]:
        cpp = item["cpp"]
        rust = item["rust"]
        escaped_cpp = escape_cpp_type(cpp)

        # Futures stuff
        future_alias = "BoxFuture" + fix_name_for_rust(rust)
        print(textwrap.dedent(f"""\
            unsafe impl {crate}::future::BoxFutureTarget for {rust} {{
                unsafe fn make_ready_future(val: *mut std::ffi::c_void) -> *mut std::ffi::c_void {{
                    extern "C" {{
                        #[link_name = "seastar_rs_future_{escaped_cpp}_make_ready_future"]
                        fn impl_fn(val: *mut std::ffi::c_void) -> *mut std::ffi::c_void;
                    }}
                    impl_fn(val)
                }}
                unsafe fn make_exception_future(eptr: *mut std::ffi::c_void) -> *mut std::ffi::c_void {{
                    extern "C" {{
                        #[link_name = "seastar_rs_future_{escaped_cpp}_make_exception_future"]
                        fn impl_fn(eptr: *mut std::ffi::c_void) -> *mut std::ffi::c_void;
                    }}
                    impl_fn(eptr)
                }}
                unsafe fn free(cpp_fut: *mut std::ffi::c_void) {{
                    extern "C" {{
                        #[link_name = "seastar_rs_future_{escaped_cpp}_free"]
                        fn impl_fn(cpp_fut: *mut std::ffi::c_void);
                    }}
                    impl_fn(cpp_fut)
                }}
                unsafe fn attach_poll_state(cpp_fut: *mut std::ffi::c_void, poll_state: *mut std::ffi::c_void) {{
                    extern "C" {{
                        #[link_name = "seastar_rs_future_{escaped_cpp}_attach_poll_state"]
                        fn impl_fn(cpp_fut: *mut std::ffi::c_void, poll_state: *mut std::ffi::c_void);
                    }}
                    impl_fn(cpp_fut, poll_state)
                }}
            }}

            unsafe impl ::cxx::ExternType for {crate}::future::BoxFuture<{rust}> {{
                type Id = ::cxx::type_id!(seastar::rs::generated::{future_alias});
                type Kind = ::cxx::kind::Trivial;
            }}

            pub type {future_alias} = {crate}::future::BoxFuture<{rust}>;

            #[no_mangle]
            pub extern "C" fn seastar_rs_future_{escaped_cpp}_future_poll_dispose(future_poll_ptr: *mut std::ffi::c_void) {{
                unsafe {{
                    {crate}::future::internal::future_poll_dispose::<{rust}>(future_poll_ptr);
                }}
            }}
            #[no_mangle]
            pub extern "C" fn seastar_rs_future_{escaped_cpp}_future_poll_wake(future_poll_ptr: *mut std::ffi::c_void) {{
                unsafe {{
                    {crate}::future::internal::future_poll_wake::<{rust}>(future_poll_ptr);
                }}
            }}
            """))

        # Promises stuff
        promise_alias = "BoxPromise" + fix_name_for_rust(rust)
        print(textwrap.dedent(f"""\
            unsafe impl {crate}::promise::BoxPromiseTarget for {rust} {{
                unsafe fn new() -> *mut std::ffi::c_void {{
                    extern "C" {{
                        #[link_name = "seastar_rs_promise_{escaped_cpp}_new"]
                        fn impl_fn() -> *mut std::ffi::c_void;
                    }}
                    impl_fn()
                }}
                unsafe fn set_value(cpp_prom: *const std::ffi::c_void, val: *mut std::ffi::c_void) {{
                    extern "C" {{
                        #[link_name = "seastar_rs_promise_{escaped_cpp}_set_value"]
                        fn impl_fn(cpp_prom: *const std::ffi::c_void, val: *mut std::ffi::c_void);
                    }}
                    impl_fn(cpp_prom, val)
                }}
                unsafe fn set_exception(cpp_prom: *const std::ffi::c_void, eptr: *mut std::ffi::c_void) {{
                    extern "C" {{
                        #[link_name = "seastar_rs_promise_{escaped_cpp}_set_exception"]
                        fn impl_fn(cpp_prom: *const std::ffi::c_void, eptr: *mut std::ffi::c_void);
                    }}
                    impl_fn(cpp_prom, eptr)
                }}
                unsafe fn get_future(cpp_prom: *const std::ffi::c_void) -> *mut std::ffi::c_void {{
                    extern "C" {{
                        #[link_name = "seastar_rs_promise_{escaped_cpp}_get_future"]
                        fn impl_fn(cpp_prom: *const std::ffi::c_void) -> *mut std::ffi::c_void;
                    }}
                    impl_fn(cpp_prom)
                }}
                unsafe fn free(cpp_prom: *mut std::ffi::c_void) {{
                    extern "C" {{
                        #[link_name = "seastar_rs_promise_{escaped_cpp}_free"]
                        fn impl_fn(cpp_prom: *mut std::ffi::c_void);
                    }}
                    impl_fn(cpp_prom)
                }}
            }}

            unsafe impl ::cxx::ExternType for {crate}::promise::BoxPromise<{rust}> {{
                type Id = ::cxx::type_id!(seastar::rs::generated::{promise_alias});
                type Kind = ::cxx::kind::Trivial;
            }}

            pub type {promise_alias} = {crate}::promise::BoxPromise<{rust}>;
            """))


def generate_cpp_futures_promises_header(y):
    license()
    print("#pragma once")
    for i in y["includes"]:
        print(f"#include {i}")
    print()
    print(f"namespace seastar::rs::generated {{")
    for item in y["items"]:
        cpp = item["cpp"]
        escaped_cpp = escape_cpp_type(cpp)
        print(f"using future_box_{escaped_cpp} = ::seastar::rs::future_box<{cpp}>;")
        print(f"using promise_box_{escaped_cpp} = ::seastar::rs::promise_box<{cpp}>;")

        # These names don't follow the seastar naming convention, but having them
        # makes it much easier to import in rust code via a cxx bridge.
        rust = item["rust"]
        future_alias = "BoxFuture" + fix_name_for_rust(rust)
        promise_alias = "BoxPromise" + fix_name_for_rust(rust)
        print(f"using {future_alias} = ::seastar::rs::future_box<{cpp}>;")
        print(f"using {promise_alias} = ::seastar::rs::promise_box<{cpp}>;")
    print(f"}} // namespace seastar::rs::generated")


def generate_cpp_futures_promises_dist(y):
    license()
    print("#pragma once")
    print("#include \"rust/cxx/internal/future_promise.hh\"")
    for i in y["includes"]:
        print(f"#include {i}")
    print()
    for item in y["items"]:
        cpp = item["cpp"]
        escaped_cpp = escape_cpp_type(cpp)

        # Futures stuff
        print(textwrap.dedent(f"""\
            extern "C" void seastar_rs_future_{escaped_cpp}_future_poll_dispose(seastar::rs::internal::future_poll<{cpp}>* fp);
            extern "C" void seastar_rs_future_{escaped_cpp}_future_poll_wake(seastar::rs::internal::future_poll<{cpp}>* fp);

            template<>
            struct seastar::rs::internal::future_poll_externs<{cpp}> {{
                static void dispose(seastar::rs::internal::future_poll<{cpp}>* fp) noexcept {{
                    seastar_rs_future_{escaped_cpp}_future_poll_dispose(fp);
                }}
                static void wake(seastar::rs::internal::future_poll<{cpp}>* fp) noexcept {{
                    seastar_rs_future_{escaped_cpp}_future_poll_wake(fp);
                }}
            }};

            extern "C" void* seastar_rs_future_{escaped_cpp}_make_ready_future({cpp}* data) noexcept {{
                return seastar::rs::internal::make_ready_future(data);
            }}
            extern "C" void* seastar_rs_future_{escaped_cpp}_make_exception_future(std::exception_ptr* data) noexcept {{
                return seastar::rs::internal::make_exception_future<{cpp}>(data);
            }}
            extern "C" void seastar_rs_future_{escaped_cpp}_free(::seastar::future<{cpp}>* fut) noexcept {{
                delete fut;
            }}
            extern "C" void seastar_rs_future_{escaped_cpp}_attach_poll_state(
                    ::seastar::future<{cpp}>* fut,
                    seastar::rs::internal::future_poll<{cpp}>* fp) noexcept {{
                seastar::rs::internal::future_attach_poll_state(fut, fp);
            }}
            """))

        # Promises stuff
        print(textwrap.dedent(f"""\
            extern "C" void* seastar_rs_promise_{escaped_cpp}_new() noexcept {{
                return (void*)new ::seastar::promise<{cpp}>();
            }}
            extern "C" void seastar_rs_promise_{escaped_cpp}_set_value(
                    ::seastar::promise<{cpp}>* prom,
                    {cpp}* value) noexcept {{
                seastar::rs::internal::promise_set_value(prom, value);
            }}
            extern "C" void seastar_rs_promise_{escaped_cpp}_set_exception(
                    ::seastar::promise<{cpp}>* prom,
                    std::exception_ptr* eptr) noexcept {{
                seastar::rs::internal::promise_set_exception<{cpp}>(prom, eptr);
            }}
            extern "C" void* seastar_rs_promise_{escaped_cpp}_get_future(::seastar::promise<{cpp}>* prom) noexcept {{
                return seastar::rs::internal::promise_get_future(prom);
            }}
            extern "C" void seastar_rs_promise_{escaped_cpp}_free(::seastar::promise<{cpp}>* prom) noexcept {{
                delete prom;
            }}
            """))


def generate_rust_exceptions(y):
    license()
    crate = get_seastar_crate_path(y)
    for item in y["items"]:
        cpp = item["cpp"]
        rust = item["rust"]
        escaped_cpp = escape_cpp_type(cpp)
        print(textwrap.dedent(f"""\
            #[repr(C)]
            pub struct {rust} {{
                _data: [u8; 0],
            }}

            impl std::fmt::Debug for {rust} {{
                fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {{
                    f.write_str("{rust}")
                }}
            }}
            impl std::fmt::Display for {rust} {{
                fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {{
                    unsafe {{
                        {crate}::exception::internal::write_exception_what(
                            self as *const _ as *const std::ffi::c_void,
                            f,
                        )
                    }}
                }}
            }}
            unsafe impl {crate}::exception::CxxExceptionPtrTarget for {rust} {{
                unsafe fn try_catch(ptr_to_eptr: *const std::ffi::c_void) -> *const std::ffi::c_void {{
                    extern "C" {{
                        #[link_name = "seastar_rs_exception_{escaped_cpp}_try_catch"]
                        fn impl_fn(ptr_to_eptr: *const std::ffi::c_void) -> *const std::ffi::c_void;
                    }}
                    impl_fn(ptr_to_eptr)
                }}
            }}

            unsafe impl ::cxx::ExternType for {rust} {{
                type Id = ::cxx::type_id!({cpp});
                type Kind = ::cxx::kind::Opaque;
            }}
            """))


def generate_cpp_exceptions_dist(y):
    license()
    for i in y["includes"]:
        print(f"#include {i}")
    print()
    for item in y["items"]:
        cpp = item["cpp"]
        escaped_cpp = escape_cpp_type(cpp)
        print(textwrap.dedent(f"""\
            extern "C" {cpp}* seastar_rs_exception_{escaped_cpp}_try_catch(std::exception_ptr* eptr) noexcept {{
                return *eptr ? ::try_catch<{cpp}>(*eptr) : nullptr;
            }}
            """))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("The tool expects exactly two arguments", file=sys.stderr)
        sys.exit(1)

    generators = {
        'futures_promises': {
            'rust': generate_rust_futures_promises,
            'cpp-header': generate_cpp_futures_promises_header,
            'cpp-dist': generate_cpp_futures_promises_dist,
        },
        'exceptions': {
            'rust': generate_rust_exceptions,
            'cpp-header': generate_empty_header,
            'cpp-dist': generate_cpp_exceptions_dist,
        },
    }

    gen_type = sys.argv[1]
    file_name = sys.argv[2]

    with open(file_name) as f:
        y = yaml.load(f, Loader=yaml.SafeLoader)

    if not isinstance(y, dict):
        print("Expected a dictionary YAML file", file=sys.stderr)
        sys.exit(1)
    
    if "generator" not in y:
        print("Missing mandatory \"generator\" key", file=sys.stderr)
        sys.exit(1)

    gen_name = y["generator"]
    if gen_name not in generators:
        print(f"Unknown generator: \"{gen_name}\"", file=sys.stderr)
        sys.exit(1)

    gen = generators[gen_name][gen_type]
    gen(y)
    sys.exit(0)
