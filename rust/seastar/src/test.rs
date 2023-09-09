#[cxx::bridge(namespace = "seastar_rs::internal")]
mod ffi {
    unsafe extern "C++" {
        include!("rust/bridge/testing.hh");

        #[cxx_name = "rust_test"]
        type RustTest;

        fn create_rust_test(
            test_name: &str,
            test_file: &str,
            test_line: u32,
            test_fn: fn() -> (),
        ) -> UniquePtr<RustTest>;
    }
}

// Used by the `seastar::test` macro.
#[doc(hidden)]
pub use ffi::create_rust_test;

// Reexport for use in `seastar::test` macro.
#[doc(hidden)]
pub use ctor::ctor;
