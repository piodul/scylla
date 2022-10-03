#pragma once

#include <memory>

#include <seastar/core/future.hh>

#include "rust/cxx.h"

namespace rust {
struct MyFuture;

struct RustTask : public seastar::continuation_base_with_promise<seastar::promise<uint32_t>, uint32_t> {
    rust::MyFuture* _rfut;
    bool _scheduled = true;

    void schedule_me();

    virtual void run_and_dispose() noexcept override;

    MyFuture& get_fut();

    RustTask();

    virtual ~RustTask();

    seastar::future<uint32_t> get_future();
};

void wake_rust_task(RustTask& task);

void schedule_callback_after_one_second(rust::Fn<void(MyFuture*)> fn, MyFuture* data);
}
