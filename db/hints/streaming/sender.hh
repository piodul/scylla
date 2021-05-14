// TODO: License

#include <cstdint>
#include <map>
#include <unordered_map>

#include "seastar/core/gate.hh"
#include "seastar/core/semaphore.hh"

#include "frozen_mutation.hh"
#include "message/messaging_service.fwd.hh"
#include "gms/inet_address.hh"
#include "utils/UUID.hh"

#pragma once

namespace db {

class database;

namespace hints {
namespace streaming {

// Manages outgoing hint streams.
class sender {
private:
    class metrics;
    class session;
    friend class session;

public:
    class stream_event_listener {
        virtual void on_begin() = 0;
        virtual void on_end() = 0;
        virtual void on_confirmed_reception(uint64_t up_to_mutation) = 0;
        virtual void on_flush(uint64_t up_to_mutation) = 0;
    };

private:
    std::unordered_map<gms::inet_address, lw_shared_ptr<session>> _sessions;
    seastar::semaphore _session_change_mutex{1};

    bool _is_running = false;
    db::database& _db;
    netw::messaging_service& _ms;

    future<lw_shared_ptr<session>> get_or_start_session(gms::inet_address destination);

    future<> send_hint_to_destination(
        gms::inet_address destination,
        const frozen_mutation_and_schema>& m,
    )

public:
    future<> start();
    future<> stop();

    bool is_running() const {
        return _is_running;
    }

    future<> send_hint(
        gms::inet_address original_destination,
        frozen_mutation_and_schema m
    );
};

}
}
}
