// TODO: License

#include <algorithm>
#include <optional>

#include "dht/token.hh"
#include "sender.hh"

// TODO: Logging

namespace db {
namespace hints {
namespace streaming {

struct sender::is_running {
    // How many mutation bytes we have sent so far
    uint64_t mutation_bytes_sent = 0;

    // How many files we sent in full
    uint64_t files_sent = 0;

    // How many files we deleted after receiving a confirmation
    // from the receiver side
    uint64_t files_deleted = 0;
};

class sender::session : public enable_lw_shared_from_this<sender::session> {
private:
    uint64_t _sent_hints_count = 0;
    uint64_t _confirmed_up_to = 0;
    std::map<uint64_t, sstring> _send_checkpoints;

    // From which hints queue the hint was sent last?
    gms::inet_address _last_hint_queue_source;

    bool _stop_requested = false;
    std::optional<future<>> _finished = make_ready_future<>();

    // The parent object - the sender.
    sender& _sender;

private:
    future<> background_worker() {
        // TODO: Implement!
        co_return;
    }

public:
    void start() {
        _finished = background_worker().finally([self = shared_from_this()] {});
    }

    future<> finish() && {
        _stop_requested = true;
        auto finished = std::move(*_finished);
        _finished.reset();
        return finished; // Is std::move needed here?
    }

    future<> send_hint(const frozen_mutation_and_schema& m) {
        // TODO: Implement!

        co_return;
    }
};

future<> sender::start() {
    // Sender sessions are created lazily,
    // no need to do anything here yet.
    assert(!_is_started);
    _is_started = true;

    co_return;
}

future<> sender::stop() {
    assert(_is_started);
    co_await parallel_for_each(_sessions, [] (auto& p) {
        return p.second->stop();
    });
    _is_started = false;

    co_return;
}

future<> sender::send_hint(gms::inet_address original_destination, frozen_mutation_and_schema m) {
    if (_is_running) {
        throw std::runtime_error("hints streaming sender is not running");
    }

    keyspace& ks = _db.find_keyspace(m.s->ks_name());
    auto& rs = ks.get_replication_strategy();
    auto token = dht::get_token(*m.s, m.fm.key());
    auto natural_endpoints = rs.get_natural_endpoints(std::move(token));

    if (std::find(natural_endpoints.begin(), natural_endpoints.end(), original_destination)) {
        // The original destination is still a part of the replica set.
        // Send the hint to this destination.
        co_await send_hint_to_destination(original_destination, m);
    } else {
        // The replica set has changed and no longer includes the original destination.
        // Send the hint to all its new owners.
        co_await parallel_for_each(natural_endpoints, [this, &m] (gms::inet_address& destination) {
            return send_hint_to_destination(destination, m);
        });
    }

    co_return;
}

future<lw_shared_ptr<session>> sender::get_or_start_session(gms::inet_address address) {
    const auto lock = seastar::get_units(_session_change_mutex, 1);
    auto [it, emplaced] = _sessions.try_emplace(destination, nullptr);
    if (!emplaced) {
        it->second = make_lw_shared<session>();
        co_await it->second->start();
    }

    co_return it->second;
}

future<> sender::send_hint_to_destination(
    gms::inet_address destination,
    const frozen_mutation_and_schema>& m,
) {
    // TODO: Filter out hints towards which we cannot send?

    co_await it->second->send_hint(m);

    co_return;
}

} // namespace streaming
} // namespace hints
} // namespace db
