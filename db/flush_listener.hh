/*
 * Copyright (C) 2021-present ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <list>
#include <iterator>
#include <memory>
#include <cassert>
#include "memtable.hh"

namespace db {

class flush_listener {
public:
    virtual void on_successful_flush(memtable::id id) = 0;
    virtual void on_failed_flush(memtable::id id) = 0;
};

class flush_listener_list {
private:
    using ptr_list = std::list<flush_listener*>;
    using ptr_list_iterator = ptr_list::iterator;

    ptr_list _listeners;

public:
    class listener_guard {
    private:
        flush_listener_list& _list;
        ptr_list_iterator _it;

        listener_guard(flush_listener_list& list, ptr_list_iterator it)
                : _list(list)
                , _it(it)
        {}

    public:
        listener_guard(listener_guard&&) = delete;
        listener_guard(const listener_guard&) = delete;
        listener_guard& operator=(listener_guard&&) = delete;
        listener_guard& operator=(const listener_guard&&) = delete;

        ~listener_guard() {
            _list._listeners.erase(_it);
        }

        friend class flush_listener_list;
    };

    using handle = std::unique_ptr<listener_guard>;

public:
    flush_listener_list() {}

    ~flush_listener_list() {
        // TODO: As an alternative to the assert, maybe we should make sure
        // that it is safe for handles to exist after the corresponding
        // flush_listener_list is destroyed?
        assert(_listeners.empty());
    }

    handle register_listener(flush_listener* l) {
        assert(l != nullptr);
        _listeners.push_back(l);

        // The constructor is private, so we can't use make_unique
        return std::unique_ptr<listener_guard>(
                new listener_guard(*this, std::prev(_listeners.end())));
    }

    void notify_successful_flush(memtable::id id) {
        for (flush_listener* l : _listeners) {
            l->on_successful_flush(id);
        }
    }

    void notify_failed_flush(memtable::id id) {
        for (flush_listener* l : _listeners) {
            l->on_failed_flush(id);
        }
    }
};

}
