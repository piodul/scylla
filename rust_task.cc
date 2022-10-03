#include <chrono>
#include <cstdio>

#include "rust_task.hh"
#include "rust/inc.hh"
#include "seastar/core/sleep.hh"

namespace rust {

void RustTask::schedule_me() {
    if (!_scheduled) {
        seastar::schedule(this);
        _scheduled = true;
    }
}

void RustTask::run_and_dispose() noexcept {
    _scheduled = false;
    uint32_t out;
    if (rust::poll_my_future(*this, out)) {
        this->_pr.set_value(out);
        delete this;
    }
}

MyFuture& RustTask::get_fut() {
    return *_rfut;
}

RustTask::RustTask() : continuation_base_with_promise(seastar::promise<uint32_t>()) {
    printf("Here I am: %p\n", this);
    _rfut = rust::create_rust_future();
}

RustTask::~RustTask() {
    rust::delete_rust_future(_rfut);
}

seastar::future<uint32_t> RustTask::get_future() {
    return _pr.get_future();
}

void wake_rust_task(RustTask& task) {
    printf("Task: %p\n", &task);
    task.schedule_me();
}

void schedule_callback_after_one_second(rust::Fn<void(MyFuture*)> fn, MyFuture* data) {
    (void)seastar::sleep(std::chrono::seconds(1)).then([fn, data] {
        fn(data);
    });
}

}
