/*
 * Copyright (C) 2015-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "token_metadata2.hh"
#include <optional>
#include "locator/snitch_base.hh"
#include "locator/abstract_replication_strategy.hh"
#include "locator/tablets.hh"
#include "log.hh"
#include "partition_range_compat.hh"
#include <unordered_map>
#include <algorithm>
#include <boost/icl/interval.hpp>
#include <boost/icl/interval_map.hpp>
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <boost/range/adaptors.hpp>
#include <seastar/core/smp.hh>
#include "utils/stall_free.hh"
#include "utils/fb_utilities.hh"

namespace locator {

static logging::logger tlogger("token_metadata2");

template <typename C, typename V>
static void remove_by_value(C& container, V value) {
    for (auto it = container.begin(); it != container.end();) {
        if (it->second == value) {
            it = container.erase(it);
        } else {
            it++;
        }
    }
}

class token_metadata_impl final {
public:
    using inet_address = host_id;
private:
    /**
     * Maintains token to endpoint map of every node in the cluster.
     * Each Token is associated with exactly one Address, but each Address may have
     * multiple tokens.  Hence, the BiMultiValMap collection.
     */
    // FIXME: have to be BiMultiValMap
    std::unordered_map<token, inet_address> _token_to_endpoint_map;

    // Track the unique set of nodes in _token_to_endpoint_map
    std::unordered_set<inet_address> _normal_token_owners;

    std::unordered_map<token, inet_address> _bootstrap_tokens;
    std::unordered_set<inet_address> _leaving_endpoints;
    // The map between the existing node to be replaced and the replacing node
    std::unordered_map<inet_address, inet_address> _replacing_endpoints;

    std::optional<topology_change_info2> _topology_change_info;

    std::vector<token> _sorted_tokens;

    tablet_metadata _tablets;

    topology _topology;

    token_metadata2::read_new_t _read_new = token_metadata2::read_new_t::no;

    long _ring_version = 0;
    static thread_local long _static_ring_version;

    // Zero means that token_metadata2 versions are not supported,
    // this will be used in RPC handling to decide whether we
    // need to apply fencing or not.
    // The initial valid version is 1;
    token_metadata2::version_t _version = 0;
    token_metadata2::version_tracker_t _version_tracker;

    // Note: if any member is added to this class
    // clone_async() must be updated to copy that member.

    void sort_tokens();

    const tablet_metadata& tablets() const { return _tablets; }
    tablet_metadata& tablets() { return _tablets; }

    void set_tablets(tablet_metadata&& tablets) {
        _tablets = std::move(tablets);
        invalidate_cached_rings();
    }

    struct shallow_copy {};
public:
    token_metadata_impl(shallow_copy, const token_metadata_impl& o) noexcept
        : _topology(topology::config{})
    {}
    token_metadata_impl(token_metadata2::config cfg) noexcept : _topology(std::move(cfg.topo_cfg)) {};
    token_metadata_impl(const token_metadata_impl&) = delete; // it's too huge for direct copy, use clone_async()
    token_metadata_impl(token_metadata_impl&&) noexcept = default;
    const std::vector<token>& sorted_tokens() const;
    future<> update_normal_tokens(std::unordered_set<token> tokens, inet_address endpoint);
    const token& first_token(const token& start) const;
    size_t first_token_index(const token& start) const;
    std::optional<host_id> get_endpoint(const token& token) const;
    std::vector<token> get_tokens(const inet_address& addr) const;
    const std::unordered_map<token, inet_address>& get_token_to_endpoint() const {
        return _token_to_endpoint_map;
    }

    const std::unordered_set<inet_address>& get_leaving_endpoints() const {
        return _leaving_endpoints;
    }

    const std::unordered_map<token, inet_address>& get_bootstrap_tokens() const {
        return _bootstrap_tokens;
    }

    void update_topology(gms::inet_address ep, std::optional<endpoint_dc_rack> opt_dr, std::optional<node::state> opt_st, std::optional<shard_id> shard_count = std::nullopt) {
        _topology.add_or_update_endpoint(ep, std::nullopt, std::move(opt_dr), std::move(opt_st), std::move(shard_count));
    }

    /**
     * Creates an iterable range of the sorted tokens starting at the token next
     * after the given one.
     *
     * @param start A token that will define the beginning of the range
     *
     * @return The requested range (see the description above)
     */
    boost::iterator_range<token_metadata2::tokens_iterator> ring_range(const token& start) const;

    boost::iterator_range<token_metadata2::tokens_iterator> ring_range(dht::ring_position_view pos) const;

    topology& get_topology() {
        return _topology;
    }

    const topology& get_topology() const {
        return _topology;
    }

    void debug_show() const;

    /**
     * Store an end-point to host ID mapping.  Each ID must be unique, and
     * cannot be changed after the fact.
     *
     * @param hostId
     * @param endpoint
     */
    void update_host_id(const locator::host_id& host_id, gms::inet_address endpoint);

    /** Return the unique host ID for an end-point. */
    host_id get_host_id(inet_address endpoint) const;

    /// Return the unique host ID for an end-point or nullopt if not found.
    std::optional<host_id> get_host_id_if_known(inet_address endpoint) const;

    /** Return the end-point for a unique host ID */
    std::optional<gms::inet_address> get_endpoint_for_host_id(host_id) const;

    /** @return a copy of the endpoint-to-id map for read-only operations */
    std::unordered_map<gms::inet_address, locator::host_id> get_endpoint_to_host_id_map_for_reading() const;

    void add_bootstrap_token(token t, inet_address endpoint);

    void add_bootstrap_tokens(std::unordered_set<token> tokens, inet_address endpoint);

    void remove_bootstrap_tokens(std::unordered_set<token> tokens);

    void add_leaving_endpoint(inet_address endpoint);
    void del_leaving_endpoint(inet_address endpoint);
public:
    void remove_endpoint(inet_address endpoint);

    bool is_normal_token_owner(inet_address endpoint) const;

    bool is_leaving(inet_address endpoint) const;

    // Is this node being replaced by another node
    bool is_being_replaced(inet_address endpoint) const;

    // Is any node being replaced by another node
    bool is_any_node_being_replaced() const;

    void add_replacing_endpoint(inet_address existing_node, inet_address replacing_node);

    void del_replacing_endpoint(inet_address existing_node);
public:

    /**
     * Create a full copy of token_metadata_impl using asynchronous continuations.
     * The caller must ensure that the cloned object will not change if
     * the function yields.
     */
    future<std::unique_ptr<token_metadata_impl>> clone_async() const noexcept;

    /**
     * Create a copy of TokenMetadata with only tokenToEndpointMap. That is, pending ranges,
     * bootstrap tokens and leaving endpoints are not included in the copy.
     * The caller must ensure that the cloned object will not change if
     * the function yields.
     */
    future<std::unique_ptr<token_metadata_impl>> clone_only_token_map(bool clone_sorted_tokens = true) const noexcept;

    /**
     * Create a copy of TokenMetadata with tokenToEndpointMap reflecting situation after all
     * current leave operations have finished.
     *
     * @return new token metadata
     */
    future<std::unique_ptr<token_metadata_impl>> clone_after_all_left() const noexcept {
        return clone_only_token_map(false).then([this] (std::unique_ptr<token_metadata_impl> all_left_metadata) {
            for (auto endpoint : _leaving_endpoints) {
                all_left_metadata->remove_endpoint(endpoint);
            }
            all_left_metadata->sort_tokens();

            return all_left_metadata;
        });
    }

    /**
     * Destroy the token_metadata2 members using continuations
     * to prevent reactor stalls.
     */
    future<> clear_gently() noexcept;

public:
    dht::token_range_vector get_primary_ranges_for(std::unordered_set<token> tokens) const;

    dht::token_range_vector get_primary_ranges_for(token right) const;
    static boost::icl::interval<token>::interval_type range_to_interval(range<dht::token> r);
    static range<dht::token> interval_to_range(boost::icl::interval<token>::interval_type i);

public:
    // future<> update_topology_change_info(dc_rack_fn& get_dc_rack);
    const std::optional<topology_change_info2>& get_topology_change_info() const {
        return _topology_change_info;
    }
public:

    token get_predecessor(token t) const;

    // Returns nodes that are officially part of the ring. It does not include
    // node that is still joining the cluster, e.g., a node that is still
    // streaming data before it finishes the bootstrap process and turns into
    // NORMAL status.
    const std::unordered_set<inet_address>& get_all_endpoints() const noexcept {
        return _normal_token_owners;
    }

    /* Returns the number of different endpoints that own tokens in the ring.
     * Bootstrapping tokens are not taken into account. */
    size_t count_normal_token_owners() const;
private:
    future<> update_normal_token_owners();
public:
    // returns empty vector if keyspace_name not found.
    inet_address_vector_topology_change pending_endpoints_for(const token& token, const sstring& keyspace_name) const;

    std::optional<inet_address_vector_replica_set> endpoints_for_reading(const token& token, const sstring& keyspace_name) const;

    void set_read_new(token_metadata2::read_new_t read_new) {
        _read_new = read_new;
    }

public:
    /** @return an endpoint to token multimap representation of tokenToEndpointMap (a copy) */
    std::multimap<inet_address, token> get_endpoint_to_token_map_for_reading() const;
    /**
     * @return a (stable copy, won't be modified) Token to Endpoint map for all the normal and bootstrapping nodes
     *         in the cluster.
     */
    std::map<token, inet_address> get_normal_and_bootstrapping_token_to_endpoint_map() const;

    long get_ring_version() const {
        return _ring_version;
    }

    void invalidate_cached_rings() {
        _ring_version = ++_static_ring_version;
        tlogger.debug("ring_version={}", _ring_version);
    }

    token_metadata2::version_t get_version() const {
        return _version;
    }
    void set_version(token_metadata2::version_t version) {
        if (version <= 0) {
            on_internal_error(tlogger,
                format("token_metadata_impl::set_version: invalid new version {}", version));
        }
        if (version < _version) {
            on_internal_error(tlogger,
                format("token_metadata_impl::set_version: new version can't be smaller than the previous one, "
                       "new version {}, previous version {}", version, _version));
        }
        _version = version;
    }
    void set_version_tracker(token_metadata2::version_tracker_t tracker) {
        _version_tracker = std::move(tracker);
    }

    friend class token_metadata2;
};

thread_local long token_metadata_impl::_static_ring_version;

token_metadata2::tokens_iterator::tokens_iterator(const token& start, const token_metadata_impl* token_metadata2)
    : _token_metadata(token_metadata2) {
    _cur_it = _token_metadata->sorted_tokens().begin() + _token_metadata->first_token_index(start);
    _remaining = _token_metadata->sorted_tokens().size();
}

bool token_metadata2::tokens_iterator::operator==(const tokens_iterator& it) const {
    return _remaining == it._remaining;
}

const token& token_metadata2::tokens_iterator::operator*() const {
    return *_cur_it;
}

token_metadata2::tokens_iterator& token_metadata2::tokens_iterator::operator++() {
    ++_cur_it;
    if (_cur_it == _token_metadata->sorted_tokens().end()) {
        _cur_it = _token_metadata->sorted_tokens().begin();
    }
    --_remaining;
    return *this;
}

host_id token_metadata2::get_my_id() const {
    return get_topology().get_config().this_host_id;
}

inline
boost::iterator_range<token_metadata2::tokens_iterator>
token_metadata_impl::ring_range(const token& start) const {
    auto begin = token_metadata2::tokens_iterator(start, this);
    auto end = token_metadata2::tokens_iterator();
    return boost::make_iterator_range(begin, end);
}

future<std::unique_ptr<token_metadata_impl>> token_metadata_impl::clone_async() const noexcept {
    auto ret = co_await clone_only_token_map();
    ret->_bootstrap_tokens.reserve(_bootstrap_tokens.size());
    for (const auto& p : _bootstrap_tokens) {
        ret->_bootstrap_tokens.emplace(p);
        co_await coroutine::maybe_yield();
    }
    ret->_leaving_endpoints = _leaving_endpoints;
    ret->_replacing_endpoints = _replacing_endpoints;
    ret->_ring_version = _ring_version;
    ret->_version = _version;
    co_return ret;
}

future<std::unique_ptr<token_metadata_impl>> token_metadata_impl::clone_only_token_map(bool clone_sorted_tokens) const noexcept {
    auto ret = std::make_unique<token_metadata_impl>(shallow_copy{}, *this);
    ret->_token_to_endpoint_map.reserve(_token_to_endpoint_map.size());
    for (const auto& p : _token_to_endpoint_map) {
        ret->_token_to_endpoint_map.emplace(p);
        co_await coroutine::maybe_yield();
    }
    ret->_normal_token_owners = _normal_token_owners;
    ret->_topology = co_await _topology.clone_gently();
    if (clone_sorted_tokens) {
        ret->_sorted_tokens = _sorted_tokens;
        co_await coroutine::maybe_yield();
    }
    ret->_tablets = _tablets;
    ret->_read_new = _read_new;
    co_return ret;
}

future<> token_metadata_impl::clear_gently() noexcept {
    co_await utils::clear_gently(_token_to_endpoint_map);
    co_await utils::clear_gently(_normal_token_owners);
    co_await utils::clear_gently(_bootstrap_tokens);
    co_await utils::clear_gently(_leaving_endpoints);
    co_await utils::clear_gently(_replacing_endpoints);
    co_await utils::clear_gently(_sorted_tokens);
    co_await _topology.clear_gently();
    co_await _tablets.clear_gently();
    co_return;
}

void token_metadata_impl::sort_tokens() {
    std::vector<token> sorted;
    sorted.reserve(_token_to_endpoint_map.size());

    for (auto&& i : _token_to_endpoint_map) {
        sorted.push_back(i.first);
    }

    std::sort(sorted.begin(), sorted.end());

    _sorted_tokens = std::move(sorted);
}

const tablet_metadata& token_metadata2::tablets() const {
    return _impl->tablets();
}

tablet_metadata& token_metadata2::tablets() {
    return _impl->tablets();
}

void token_metadata2::set_tablets(tablet_metadata tm) {
    _impl->set_tablets(std::move(tm));
}

const std::vector<token>& token_metadata_impl::sorted_tokens() const {
    return _sorted_tokens;
}

std::vector<token> token_metadata_impl::get_tokens(const inet_address& addr) const {
    std::vector<token> res;
    for (auto&& i : _token_to_endpoint_map) {
        if (i.second == addr) {
            res.push_back(i.first);
        }
    }
    std::sort(res.begin(), res.end());
    return res;
}

future<> token_metadata_impl::update_normal_tokens(std::unordered_set<token> tokens, inet_address endpoint) {
    if (tokens.empty()) {
        co_return;
    }

    if (!_topology.has_node(endpoint)) {
        on_internal_error(tlogger, format("token_metadata_impl: {} must be a member of topology to update normal tokens", endpoint));
    }

    bool should_sort_tokens = false;

    // Phase 1: erase all tokens previously owned by the endpoint.
    for(auto it = _token_to_endpoint_map.begin(), ite = _token_to_endpoint_map.end(); it != ite;) {
        co_await coroutine::maybe_yield();
        if(it->second == endpoint) {
            auto tokit = tokens.find(it->first);
            if (tokit == tokens.end()) {
                // token no longer owned by endpoint
                it = _token_to_endpoint_map.erase(it);
                continue;
            }
            // token ownership did not change,
            // no further update needed for it.
            tokens.erase(tokit);
        }
        ++it;
    }

    // Phase 2:
    // a. ...
    // b. update pending _bootstrap_tokens and _leaving_endpoints
    // c. update _token_to_endpoint_map with the new endpoint->token mappings
    //    - set `should_sort_tokens` if new tokens were added
    remove_by_value(_bootstrap_tokens, endpoint);
    _leaving_endpoints.erase(endpoint);
    invalidate_cached_rings();
    for (const token& t : tokens)
    {
        co_await coroutine::maybe_yield();
        auto prev = _token_to_endpoint_map.insert(std::pair<token, inet_address>(t, endpoint));
        should_sort_tokens |= prev.second; // new token inserted -> sort
        if (prev.first->second != endpoint) {
            tlogger.debug("Token {} changing ownership from {} to {}", t, prev.first->second, endpoint);
            prev.first->second = endpoint;
        }
    }

    co_await update_normal_token_owners();

    // New tokens were added to _token_to_endpoint_map
    // so re-sort all tokens.
    if (should_sort_tokens) {
        sort_tokens();
    }
    co_return;
}

size_t token_metadata_impl::first_token_index(const token& start) const {
    if (_sorted_tokens.empty()) {
        auto msg = format("sorted_tokens is empty in first_token_index!");
        tlogger.error("{}", msg);
        throw std::runtime_error(msg);
    }
    auto it = std::lower_bound(_sorted_tokens.begin(), _sorted_tokens.end(), start);
    if (it == _sorted_tokens.end()) {
        return 0;
    } else {
        return std::distance(_sorted_tokens.begin(), it);
    }
}

const token& token_metadata_impl::first_token(const token& start) const {
    return _sorted_tokens[first_token_index(start)];
}

auto token_metadata_impl::get_endpoint(const token& token) const -> std::optional<host_id> {
    auto it = _token_to_endpoint_map.find(token);
    if (it == _token_to_endpoint_map.end()) {
        return std::nullopt;
    } else {
        return it->second;
    }
}

void token_metadata_impl::debug_show() const {
    auto reporter = std::make_shared<timer<lowres_clock>>();
    reporter->set_callback ([reporter, this] {
        fmt::print("Endpoint -> Token\n");
        for (auto x : _token_to_endpoint_map) {
            fmt::print("inet_address={}, token={}\n", x.second, x.first);
        }
        fmt::print("Sorted Token\n");
        for (auto x : _sorted_tokens) {
            fmt::print("token={}\n", x);
        }
    });
    reporter->arm_periodic(std::chrono::seconds(1));
}

void token_metadata_impl::update_host_id(const locator::host_id& host_id, gms::inet_address endpoint) {
    // _topology.add_or_update_endpoint(endpoint, host_id);
}

auto token_metadata_impl::get_host_id(inet_address endpoint) const -> host_id {
    if (const auto* node = _topology.find_node(endpoint)) [[likely]] {
        return node->host_id();
    } else {
        throw std::runtime_error(format("host_id for endpoint {} is not found", endpoint));
    }
}

auto token_metadata_impl::get_host_id_if_known(inet_address endpoint) const -> std::optional<inet_address>{
    if (const auto* node = _topology.find_node(endpoint)) [[likely]] {
        return node->host_id();
    } else {
        return std::nullopt;
    }
}

auto token_metadata_impl::get_endpoint_for_host_id(host_id host_id) const -> std::optional<gms::inet_address> {
    if (const auto* node = _topology.find_node(host_id)) [[likely]] {
        return node->endpoint();
    } else {
        return std::nullopt;
    }
}

// FIXME: This can be moved to `topology`
auto token_metadata_impl::get_endpoint_to_host_id_map_for_reading() const -> std::unordered_map<inet_address, host_id> {
    const auto& nodes = _topology.get_nodes_by_endpoint();
    std::unordered_map<inet_address, host_id> map;
    map.reserve(nodes.size());
    for (const auto& [endpoint, node] : nodes) {
        // Restrict to token-owners
        if (!(node->is_normal() || node->is_leaving())) {
            continue;
        }
        if (const auto& host_id = node->host_id()) {
            map[endpoint] = host_id;
        } else {
            tlogger.info("get_endpoint_to_host_id_map_for_reading: endpoint {} has null host_id: state={}", endpoint, node->get_state());
        }
    }
    return map;
}

bool token_metadata_impl::is_normal_token_owner(inet_address endpoint) const {
    return _normal_token_owners.contains(endpoint);
}

void token_metadata_impl::add_bootstrap_token(token t, inet_address endpoint) {
    std::unordered_set<token> tokens{t};
    add_bootstrap_tokens(tokens, endpoint);
}

boost::iterator_range<token_metadata2::tokens_iterator>
token_metadata_impl::ring_range(const dht::ring_position_view start) const {
    return ring_range(start.token());
}

void token_metadata_impl::add_bootstrap_tokens(std::unordered_set<token> tokens, inet_address endpoint) {
    for (auto t : tokens) {
        auto old_endpoint = _bootstrap_tokens.find(t);
        if (old_endpoint != _bootstrap_tokens.end() && (*old_endpoint).second != endpoint) {
            auto msg = format("Bootstrap Token collision between {} and {} (token {}", (*old_endpoint).second, endpoint, t);
            throw std::runtime_error(msg);
        }

        auto old_endpoint2 = _token_to_endpoint_map.find(t);
        if (old_endpoint2 != _token_to_endpoint_map.end() && (*old_endpoint2).second != endpoint) {
            auto msg = format("Bootstrap Token collision between {} and {} (token {}", (*old_endpoint2).second, endpoint, t);
            throw std::runtime_error(msg);
        }
    }

    std::erase_if(_bootstrap_tokens, [endpoint] (const std::pair<token, inet_address>& n) { return n.second == endpoint; });

    for (auto t : tokens) {
        _bootstrap_tokens[t] = endpoint;
    }
}

void token_metadata_impl::remove_bootstrap_tokens(std::unordered_set<token> tokens) {
    if (tokens.empty()) {
        tlogger.warn("tokens is empty in remove_bootstrap_tokens!");
        return;
    }
    for (auto t : tokens) {
        _bootstrap_tokens.erase(t);
    }
}

bool token_metadata_impl::is_leaving(inet_address endpoint) const {
    return _leaving_endpoints.contains(endpoint);
}

bool token_metadata_impl::is_being_replaced(inet_address endpoint) const {
    return _replacing_endpoints.contains(endpoint);
}

bool token_metadata_impl::is_any_node_being_replaced() const {
    return !_replacing_endpoints.empty();
}

void token_metadata_impl::remove_endpoint(inet_address endpoint) {
    remove_by_value(_bootstrap_tokens, endpoint);
    remove_by_value(_token_to_endpoint_map, endpoint);
    _normal_token_owners.erase(endpoint);
    _topology.remove_endpoint(endpoint);
    _leaving_endpoints.erase(endpoint);
    del_replacing_endpoint(endpoint);
    invalidate_cached_rings();
}

token token_metadata_impl::get_predecessor(token t) const {
    auto& tokens = sorted_tokens();
    auto it = std::lower_bound(tokens.begin(), tokens.end(), t);
    if (it == tokens.end() || *it != t) {
        auto msg = format("token error in get_predecessor!");
        tlogger.error("{}", msg);
        throw std::runtime_error(msg);
    }
    if (it == tokens.begin()) {
        // If the token is the first element, its preprocessor is the last element
        return tokens.back();
    } else {
        return *(--it);
    }
}

dht::token_range_vector token_metadata_impl::get_primary_ranges_for(std::unordered_set<token> tokens) const {
    dht::token_range_vector ranges;
    ranges.reserve(tokens.size() + 1); // one of the ranges will wrap
    for (auto right : tokens) {
        auto left = get_predecessor(right);
        ::compat::unwrap_into(
                wrapping_range<token>(range_bound<token>(left, false), range_bound<token>(right)),
                dht::token_comparator(),
                [&] (auto&& rng) { ranges.push_back(std::move(rng)); });
    }
    return ranges;
}

dht::token_range_vector token_metadata_impl::get_primary_ranges_for(token right) const {
    return get_primary_ranges_for(std::unordered_set<token>{right});
}

boost::icl::interval<token>::interval_type
token_metadata_impl::range_to_interval(range<dht::token> r) {
    bool start_inclusive = false;
    bool end_inclusive = false;
    token start = dht::minimum_token();
    token end = dht::maximum_token();

    if (r.start()) {
        start = r.start()->value();
        start_inclusive = r.start()->is_inclusive();
    }

    if (r.end()) {
        end = r.end()->value();
        end_inclusive = r.end()->is_inclusive();
    }

    if (start_inclusive == false && end_inclusive == false) {
        return boost::icl::interval<token>::open(std::move(start), std::move(end));
    } else if (start_inclusive == false && end_inclusive == true) {
        return boost::icl::interval<token>::left_open(std::move(start), std::move(end));
    } else if (start_inclusive == true && end_inclusive == false) {
        return boost::icl::interval<token>::right_open(std::move(start), std::move(end));
    } else {
        return boost::icl::interval<token>::closed(std::move(start), std::move(end));
    }
}

range<dht::token>
token_metadata_impl::interval_to_range(boost::icl::interval<token>::interval_type i) {
    bool start_inclusive;
    bool end_inclusive;
    auto bounds = i.bounds().bits();
    if (bounds == boost::icl::interval_bounds::static_open) {
        start_inclusive = false;
        end_inclusive = false;
    } else if (bounds == boost::icl::interval_bounds::static_left_open) {
        start_inclusive = false;
        end_inclusive = true;
    } else if (bounds == boost::icl::interval_bounds::static_right_open) {
        start_inclusive = true;
        end_inclusive = false;
    } else if (bounds == boost::icl::interval_bounds::static_closed) {
        start_inclusive = true;
        end_inclusive = true;
    } else {
        throw std::runtime_error("Invalid boost::icl::interval<token> bounds");
    }
    return range<dht::token>({{i.lower(), start_inclusive}}, {{i.upper(), end_inclusive}});
}

// future<> token_metadata_impl::update_topology_change_info(dc_rack_fn& get_dc_rack) {
//     if (_bootstrap_tokens.empty() && _leaving_endpoints.empty() && _replacing_endpoints.empty()) {
//         co_await utils::clear_gently(_topology_change_info);
//         _topology_change_info.reset();
//         co_return;
//     }

//     // true if there is a node replaced with the same IP
//     bool replace_with_same_endpoint = false;
//     // target_token_metadata incorporates all the changes from leaving, bootstrapping and replacing
//     auto target_token_metadata = co_await clone_only_token_map(false);
//     {
//         // construct new_normal_tokens based on _bootstrap_tokens and _replacing_endpoints
//         std::unordered_map<inet_address, std::unordered_set<token>> new_normal_tokens;
//         if (!_replacing_endpoints.empty()) {
//             for (const auto& [token, inet_address]: _token_to_endpoint_map) {
//                 const auto it = _replacing_endpoints.find(inet_address);
//                 if (it == _replacing_endpoints.end()) {
//                     continue;
//                 }
//                 new_normal_tokens[it->second].insert(token);
//             }
//             for (const auto& [replace_from, replace_to]: _replacing_endpoints) {
//                 if (replace_from == replace_to) {
//                     replace_with_same_endpoint = true;
//                 } else {
//                     target_token_metadata->remove_endpoint(replace_from);
//                 }
//             }
//         }
//         for (const auto& [token, inet_address]: _bootstrap_tokens) {
//             new_normal_tokens[inet_address].insert(token);
//         }
//         // apply new_normal_tokens
//         for (auto& [endpoint, tokens]: new_normal_tokens) {
//             target_token_metadata->update_topology(endpoint, get_dc_rack(endpoint), node::state::normal);
//             co_await target_token_metadata->update_normal_tokens(std::move(tokens), endpoint);
//         }
//         // apply leaving endpoints
//         for (const auto& endpoint: _leaving_endpoints) {
//             target_token_metadata->remove_endpoint(endpoint);
//         }
//         target_token_metadata->sort_tokens();
//     }

//     // We require a distinct token_metadata2 instance when replace_from equals replace_to,
//     // as it ensures the node is included in pending_ranges.
//     // Otherwise, the node would be excluded from both pending_ranges and
//     // get_natural_endpoints_without_node_being_replaced,
//     // causing the coordinator to overlook it entirely.
//     std::unique_ptr<token_metadata_impl> base_token_metadata;
//     if (replace_with_same_endpoint) {
//         base_token_metadata = co_await clone_only_token_map(false);
//         for (const auto& [replace_from, replace_to]: _replacing_endpoints) {
//             if (replace_from == replace_to) {
//                 base_token_metadata->remove_endpoint(replace_from);
//             }
//         }
//         base_token_metadata->sort_tokens();
//     }

//     // merge tokens from token_to_endpoint and bootstrap_tokens,
//     // preserving tokens of leaving endpoints
//     auto all_tokens = std::vector<dht::token>();
//     all_tokens.reserve(sorted_tokens().size() + get_bootstrap_tokens().size());
//     all_tokens.resize(sorted_tokens().size());
//     std::copy(begin(sorted_tokens()), end(sorted_tokens()), begin(all_tokens));
//     for (const auto& p: get_bootstrap_tokens()) {
//         all_tokens.push_back(p.first);
//     }
//     std::sort(begin(all_tokens), end(all_tokens));

//     auto prev_value = std::move(_topology_change_info);
//     _topology_change_info.emplace(token_metadata2(std::move(target_token_metadata)),
//         base_token_metadata ? std::optional(token_metadata2(std::move(base_token_metadata))): std::nullopt,
//         std::move(all_tokens),
//         _read_new);
//     co_await utils::clear_gently(prev_value);
// }

size_t token_metadata_impl::count_normal_token_owners() const {
    return _normal_token_owners.size();
}

future<> token_metadata_impl::update_normal_token_owners() {
    std::unordered_set<inet_address> eps;
    for (auto [t, ep]: _token_to_endpoint_map) {
        eps.insert(ep);
        co_await coroutine::maybe_yield();
    }
    _normal_token_owners = std::move(eps);
}

void token_metadata_impl::add_leaving_endpoint(inet_address endpoint) {
     _leaving_endpoints.emplace(endpoint);
}

void token_metadata_impl::del_leaving_endpoint(inet_address endpoint) {
     _leaving_endpoints.erase(endpoint);
}

void token_metadata_impl::add_replacing_endpoint(inet_address existing_node, inet_address replacing_node) {
    tlogger.info("Added node {} as pending replacing endpoint which replaces existing node {}",
            replacing_node, existing_node);
    _replacing_endpoints[existing_node] = replacing_node;
}

void token_metadata_impl::del_replacing_endpoint(inet_address existing_node) {
    if (_replacing_endpoints.contains(existing_node)) {
        tlogger.info("Removed node {} as pending replacing endpoint which replaces existing node {}",
                _replacing_endpoints[existing_node], existing_node);
    }
    _replacing_endpoints.erase(existing_node);
}

std::map<token, inet_address> token_metadata_impl::get_normal_and_bootstrapping_token_to_endpoint_map() const {
    std::map<token, inet_address> ret(_token_to_endpoint_map.begin(), _token_to_endpoint_map.end());
    ret.insert(_bootstrap_tokens.begin(), _bootstrap_tokens.end());
    return ret;
}

std::multimap<inet_address, token> token_metadata_impl::get_endpoint_to_token_map_for_reading() const {
    std::multimap<inet_address, token> cloned;
    for (const auto& x : _token_to_endpoint_map) {
        cloned.emplace(x.second, x.first);
    }
    return cloned;
}

topology_change_info2::topology_change_info2(token_metadata2 target_token_metadata_,
        std::optional<token_metadata2> base_token_metadata_,
        std::vector<dht::token> all_tokens_,
        token_metadata2::read_new_t read_new_)
    : target_token_metadata(std::move(target_token_metadata_))
    , base_token_metadata(std::move(base_token_metadata_))
    , all_tokens(std::move(all_tokens_))
    , read_new(read_new_)
{
}

future<> topology_change_info2::clear_gently() {
    co_await utils::clear_gently(target_token_metadata);
    co_await utils::clear_gently(base_token_metadata);
    co_await utils::clear_gently(all_tokens);
}

token_metadata2::token_metadata2(std::unique_ptr<token_metadata_impl> impl)
    : _impl(std::move(impl)) {
}

token_metadata2::token_metadata2(config cfg)
        : _impl(std::make_unique<token_metadata_impl>(std::move(cfg))) {
}

token_metadata2::~token_metadata2() = default;


token_metadata2::token_metadata2(token_metadata2&&) noexcept = default;

token_metadata2& token_metadata2::token_metadata2::operator=(token_metadata2&&) noexcept = default;

const std::vector<token>&
token_metadata2::sorted_tokens() const {
    return _impl->sorted_tokens();
}

future<>
token_metadata2::update_normal_tokens(std::unordered_set<token> tokens, inet_address endpoint) {
    return _impl->update_normal_tokens(std::move(tokens), endpoint);
}

const token&
token_metadata2::first_token(const token& start) const {
    return _impl->first_token(start);
}

size_t
token_metadata2::first_token_index(const token& start) const {
    return _impl->first_token_index(start);
}

std::optional<inet_address>
token_metadata2::get_endpoint(const token& token) const {
    return _impl->get_endpoint(token);
}

std::vector<token>
token_metadata2::get_tokens(const inet_address& addr) const {
    return _impl->get_tokens(addr);
}

const std::unordered_map<token, inet_address>&
token_metadata2::get_token_to_endpoint() const {
    return _impl->get_token_to_endpoint();
}

const std::unordered_set<inet_address>&
token_metadata2::get_leaving_endpoints() const {
    return _impl->get_leaving_endpoints();
}

const std::unordered_map<token, inet_address>&
token_metadata2::get_bootstrap_tokens() const {
    return _impl->get_bootstrap_tokens();
}

void
token_metadata2::update_topology(inet_address ep, std::optional<endpoint_dc_rack> opt_dr, std::optional<node::state> opt_st, std::optional<shard_id> shard_count) {
    _impl->update_topology(ep, std::move(opt_dr), std::move(opt_st), std::move(shard_count));
}

boost::iterator_range<token_metadata2::tokens_iterator>
token_metadata2::ring_range(const token& start) const {
    return _impl->ring_range(start);
}

boost::iterator_range<token_metadata2::tokens_iterator>
token_metadata2::ring_range(dht::ring_position_view start) const {
    return _impl->ring_range(start);
}

class token_metadata_ring_splitter : public locator::token_range_splitter {
    token_metadata_ptr2 _tmptr;
    boost::iterator_range<token_metadata2::tokens_iterator> _range;
public:
    token_metadata_ring_splitter(token_metadata_ptr2 tmptr)
        : _tmptr(std::move(tmptr))
        , _range(_tmptr->sorted_tokens().empty() // ring_range() throws if the ring is empty
                ? boost::make_iterator_range(token_metadata2::tokens_iterator(), token_metadata2::tokens_iterator())
                : _tmptr->ring_range(dht::minimum_token()))
    { }

    void reset(dht::ring_position_view pos) override {
        _range = _tmptr->ring_range(pos);
    }

    std::optional<dht::token> next_token() override {
        if (_range.empty()) {
            return std::nullopt;
        }
        auto t = *_range.begin();
        _range.drop_front();
        return t;
    }
};

// std::unique_ptr<locator::token_range_splitter> make_splitter(token_metadata_ptr2 tmptr) {
//     return std::make_unique<token_metadata_ring_splitter>(std::move(tmptr));
// }

topology&
token_metadata2::get_topology() {
    return _impl->get_topology();
}

const topology&
token_metadata2::get_topology() const {
    return _impl->get_topology();
}

void
token_metadata2::debug_show() const {
    _impl->debug_show();
}

void
token_metadata2::update_host_id(const host_id& host_id, inet_address endpoint) {
    _impl->update_host_id(host_id, endpoint);
}

host_id
token_metadata2::get_host_id(inet_address endpoint) const {
    return _impl->get_host_id(endpoint);
}

std::optional<host_id>
token_metadata2::get_host_id_if_known(inet_address endpoint) const {
    return _impl->get_host_id_if_known(endpoint);
}

std::optional<gms::inet_address>
token_metadata2::get_endpoint_for_host_id(host_id host_id) const {
    return _impl->get_endpoint_for_host_id(host_id);
}

host_id_or_endpoint2 token_metadata2::parse_host_id_and_endpoint(const sstring& host_id_string) const {
    auto res = host_id_or_endpoint2(host_id_string);
    res.resolve(*this);
    return res;
}

std::unordered_map<gms::inet_address, locator::host_id>
token_metadata2::get_endpoint_to_host_id_map_for_reading() const {
    return _impl->get_endpoint_to_host_id_map_for_reading();
}

void
token_metadata2::add_bootstrap_token(token t, inet_address endpoint) {
    _impl->add_bootstrap_token(t, endpoint);
}

void
token_metadata2::add_bootstrap_tokens(std::unordered_set<token> tokens, inet_address endpoint) {
    _impl->add_bootstrap_tokens(std::move(tokens), endpoint);
}

void
token_metadata2::remove_bootstrap_tokens(std::unordered_set<token> tokens) {
    _impl->remove_bootstrap_tokens(std::move(tokens));
}

void
token_metadata2::add_leaving_endpoint(inet_address endpoint) {
    _impl->add_leaving_endpoint(endpoint);
}

void
token_metadata2::del_leaving_endpoint(inet_address endpoint) {
    _impl->del_leaving_endpoint(endpoint);
}

void
token_metadata2::remove_endpoint(inet_address endpoint) {
    _impl->remove_endpoint(endpoint);
    _impl->sort_tokens();
}

bool
token_metadata2::is_normal_token_owner(inet_address endpoint) const {
    return _impl->is_normal_token_owner(endpoint);
}

bool
token_metadata2::is_leaving(inet_address endpoint) const {
    return _impl->is_leaving(endpoint);
}

bool
token_metadata2::is_being_replaced(inet_address endpoint) const {
    return _impl->is_being_replaced(endpoint);
}

bool
token_metadata2::is_any_node_being_replaced() const {
    return _impl->is_any_node_being_replaced();
}

void token_metadata2::add_replacing_endpoint(inet_address existing_node, inet_address replacing_node) {
    _impl->add_replacing_endpoint(existing_node, replacing_node);
}

void token_metadata2::del_replacing_endpoint(inet_address existing_node) {
    _impl->del_replacing_endpoint(existing_node);
}

future<token_metadata2> token_metadata2::clone_async() const noexcept {
    return _impl->clone_async().then([] (std::unique_ptr<token_metadata_impl> impl) {
        return make_ready_future<token_metadata2>(std::move(impl));
    });
}

future<token_metadata2>
token_metadata2::clone_only_token_map() const noexcept {
    return _impl->clone_only_token_map().then([] (std::unique_ptr<token_metadata_impl> impl) {
        return token_metadata2(std::move(impl));
    });
}

future<token_metadata2>
token_metadata2::clone_after_all_left() const noexcept {
    return _impl->clone_after_all_left().then([] (std::unique_ptr<token_metadata_impl> impl) {
        return token_metadata2(std::move(impl));
    });
}

future<> token_metadata2::clear_gently() noexcept {
    return _impl->clear_gently();
}

dht::token_range_vector
token_metadata2::get_primary_ranges_for(std::unordered_set<token> tokens) const {
    return _impl->get_primary_ranges_for(std::move(tokens));
}

dht::token_range_vector
token_metadata2::get_primary_ranges_for(token right) const {
    return _impl->get_primary_ranges_for(right);
}

boost::icl::interval<token>::interval_type
token_metadata2::range_to_interval(range<dht::token> r) {
    return token_metadata_impl::range_to_interval(std::move(r));
}

range<dht::token>
token_metadata2::interval_to_range(boost::icl::interval<token>::interval_type i) {
    return token_metadata_impl::interval_to_range(std::move(i));
}

// future<>
// token_metadata2::update_topology_change_info(dc_rack_fn& get_dc_rack) {
//     return _impl->update_topology_change_info(get_dc_rack);
// }

const std::optional<topology_change_info2>&
token_metadata2::get_topology_change_info() const {
    return _impl->get_topology_change_info();
}

token
token_metadata2::get_predecessor(token t) const {
    return _impl->get_predecessor(t);
}

const std::unordered_set<inet_address>&
token_metadata2::get_all_endpoints() const {
    return _impl->get_all_endpoints();
}

size_t
token_metadata2::count_normal_token_owners() const {
    return _impl->count_normal_token_owners();
}

void
token_metadata2::set_read_new(read_new_t read_new) {
    _impl->set_read_new(read_new);
}

std::multimap<inet_address, token>
token_metadata2::get_endpoint_to_token_map_for_reading() const {
    return _impl->get_endpoint_to_token_map_for_reading();
}

std::map<token, inet_address>
token_metadata2::get_normal_and_bootstrapping_token_to_endpoint_map() const {
    return _impl->get_normal_and_bootstrapping_token_to_endpoint_map();
}

long
token_metadata2::get_ring_version() const {
    return _impl->get_ring_version();
}

void
token_metadata2::invalidate_cached_rings() {
    _impl->invalidate_cached_rings();
}

auto
token_metadata2::get_version() const -> version_t {
    return _impl->get_version();
}
void
token_metadata2::set_version(version_t version) {
    _impl->set_version(version);
}
void
token_metadata2::set_version_tracker(version_tracker_t tracker) {
    _impl->set_version_tracker(std::move(tracker));
}

void shared_token_metadata::set(mutable_token_metadata_ptr2 tmptr) noexcept {
    if (_shared->get_ring_version() >= tmptr->get_ring_version()) {
        on_internal_error(tlogger, format("shared_token_metadata: must not set non-increasing ring_version: {} -> {}", _shared->get_ring_version(), tmptr->get_ring_version()));
    }

    if (_shared->get_version() > tmptr->get_version()) {
        on_internal_error(tlogger, format("shared_token_metadata: must not set decreasing version: {} -> {}", _shared->get_version(), tmptr->get_version()));
    } else if (_shared->get_version() < tmptr->get_version()) {
        _stale_versions_in_use = _versions_barrier.advance_and_await();
    }

    _shared = std::move(tmptr);
    _shared->set_version_tracker(_versions_barrier.start());
    tlogger.debug("new token_metadata2 is set, version {}", _shared->get_version());
}

void shared_token_metadata::update_fence_version(token_metadata2::version_t version) {
    if (const auto current_version = _shared->get_version(); version > current_version) {
        // The token_metadata2::version under no circumstance can go backwards.
        // Even in case of topology change coordinator moving to another node
        // this condition must hold, that is why we treat its violation
        // as an internal error.
        on_internal_error(tlogger,
            format("shared_token_metadata: invalid new fence version, can't be greater than the current version, "
                   "current version {}, new fence version {}", current_version, version));
    }
    if (version < _fence_version) {
        // If topology change coordinator moved to another node,
        // it can get ahead and increment the fence version
        // while we are handling raft_topology_cmd::command::fence,
        // so we just throw an error in this case.
        throw std::runtime_error(
            format("shared_token_metadata: can't set decreasing fence version: {} -> {}",
                _fence_version, version));
    }
    _fence_version = version;
    tlogger.debug("new fence_version is set, version {}", _fence_version);
}

future<> shared_token_metadata::mutate_token_metadata(seastar::noncopyable_function<future<> (token_metadata2&)> func) {
    auto lk = co_await get_lock();
    auto tm = co_await _shared->clone_async();
    // bump the token_metadata2 ring_version
    // to invalidate cached token/replication mappings
    // when the modified token_metadata2 is committed.
    tm.invalidate_cached_rings();
    co_await func(tm);
    set(make_token_metadata_ptr2(std::move(tm)));
}

future<> shared_token_metadata::mutate_on_all_shards(sharded<shared_token_metadata>& stm, seastar::noncopyable_function<future<> (token_metadata2&)> func) {
    auto base_shard = this_shard_id();
    assert(base_shard == 0);
    auto lk = co_await stm.local().get_lock();

    std::vector<mutable_token_metadata_ptr2> pending_token_metadata_ptr;
    pending_token_metadata_ptr.resize(smp::count);
    auto tmptr = make_token_metadata_ptr2(co_await stm.local().get()->clone_async());
    auto& tm = *tmptr;
    // bump the token_metadata2 ring_version
    // to invalidate cached token/replication mappings
    // when the modified token_metadata2 is committed.
    tm.invalidate_cached_rings();
    co_await func(tm);

    // Apply the mutated token_metadata2 only after successfully cloning it on all shards.
    pending_token_metadata_ptr[base_shard] = tmptr;
    co_await smp::invoke_on_others(base_shard, [&] () -> future<> {
        pending_token_metadata_ptr[this_shard_id()] = make_token_metadata_ptr2(co_await tm.clone_async());
    });

    co_await stm.invoke_on_all([&] (shared_token_metadata& stm) {
        stm.set(std::move(pending_token_metadata_ptr[this_shard_id()]));
    });
}

host_id_or_endpoint2::host_id_or_endpoint2(const sstring& s, param_type restrict) {
    switch (restrict) {
    case param_type::host_id:
        try {
            id = host_id(utils::UUID(s));
        } catch (const marshal_exception& e) {
            throw std::invalid_argument(format("Invalid host_id {}: {}", s, e.what()));
        }
        break;
    case param_type::endpoint:
        try {
            endpoint = gms::inet_address(s);
        } catch (std::invalid_argument& e) {
            throw std::invalid_argument(format("Invalid inet_address {}: {}", s, e.what()));
        }
        break;
    case param_type::auto_detect:
        try {
            id = host_id(utils::UUID(s));
        } catch (const marshal_exception& e) {
            try {
                endpoint = gms::inet_address(s);
            } catch (std::invalid_argument& e) {
                throw std::invalid_argument(format("Invalid host_id or inet_address {}", s));
            }
        }
    }
}

void host_id_or_endpoint2::resolve(const token_metadata2& tm) {
    if (id) {
        auto endpoint_opt = tm.get_endpoint_for_host_id(id);
        if (!endpoint_opt) {
            throw std::runtime_error(format("Host ID {} not found in the cluster", id));
        }
        endpoint = *endpoint_opt;
    } else {
        auto opt_id = tm.get_host_id_if_known(endpoint);
        if (!opt_id) {
            throw std::runtime_error(format("Host inet address {} not found in the cluster", endpoint));
        }
        id = *opt_id;
    }
}

} // namespace locator
