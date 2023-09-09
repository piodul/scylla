#pragma once

#include <memory>
#include "rust/cxx.h"

#undef SEASTAR_TESTING_MAIN
#include <seastar/testing/seastar_test.hh>

namespace seastar_rs {
namespace internal {

class rust_test final : public seastar::testing::seastar_test {
private:
    rust::Fn<void()> _impl;

public:
    rust_test(const char* test_name, const char* test_file, int test_line,
            rust::Fn<void()> test_fn);
    virtual seastar::future<> run_test_case() const override;
};

std::unique_ptr<rust_test> create_rust_test(
        rust::Str test_name, rust::Str test_file, uint32_t test_line,
        rust::Fn<void()> test_fn);

} // namespace internal
} // namespace seastar_rs
