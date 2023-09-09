#pragma once

#include <array>
#include <seastar/core/future.hh>



// namespace rust {
// namespace internal {



// }
// }

// struct RustVoidTask final : public seastar::task {
// private:
//     // Fat pointer
//     std::array<std::uintptr_t, 2> fut;
//     seastar::promise<void> p;

//     RustVoidTask(std::array<std::uintptr_t, 2> fut)
//             : fut(fut) {
//         // ???
//     }

//     // No need for a destructor, the poll function in run_and_dispose
//     // takes care of that; the assumption is that the futures are always
//     // polled to completion.

// public:
//     void run_and_dispose() noexcept override {
//         seastar_rs_RustVoidFuture_poll()
//     }

//     task* waiting_task() noexcept override {
//         return p.waiting_task();
//     }

//     seastar::future<void> get_future() {
//         return p.get_future();
//     }

//     friend class RustVoidFuture;
// };

// struct RustVoidFuture {
// private:
//     // Fat pointer
//     std::array<std::uintptr_t, 2> repr;

//     void clear() {
//         repr[0] = 0;
//         repr[1] = 0;
//     }

// public:
//     operator seastar::future<void>() && {
//         auto* t = new RustVoidTask(repr);
//         clear();
//         auto fut = t->get_future();
//         t->run_and_dispose();
//         return fut;
//     }
// };
