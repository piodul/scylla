#include "testing.hh"

namespace seastar_rs {
namespace internal {

rust_test::rust_test(const char* test_name, const char* test_file, int test_line,
        rust::Fn<void()> test_fn)
        : seastar::testing::seastar_test(test_name, test_file, test_line)
        , _impl(test_fn) {
}

seastar::future<> rust_test::run_test_case() const {
    // TODO: asynchronous stuff
    _impl();
    return seastar::make_ready_future<>();
}

std::unique_ptr<rust_test> create_rust_test(
        rust::Str test_name, rust::Str test_file, int32_t test_line,
        rust::Fn<void()> test_fn) {
    return std::make_unique<rust_test>(
            ((std::string)test_name).c_str(),
            ((std::string)test_file).c_str(),
            (int)test_line,
            test_fn);
}

}
}
