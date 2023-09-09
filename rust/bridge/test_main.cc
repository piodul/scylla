#include <seastar/testing/entry_point.hh>

// We are not using Rust's testing framework to run tests. Instead, we are
// compiling each test suite to a library and then we link against this file
// to provide an entry point.

int main(int argc, char** argv) {
    return seastar::testing::entry_point(argc, argv);
}
