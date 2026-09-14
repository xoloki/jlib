/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2026 Joey Yandle <xoloki@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */
#include <jlib/sys/reactor_backend.hh>

#include <cstdlib>
#include <iostream>

namespace jlib {
namespace sys {

constexpr std::chrono::nanoseconds reactor::forever;

reactor::work::work(reactor& r) : m_reactor(r) { m_reactor.m_work++; }

reactor::work::~work() { m_reactor.m_work--; }

reactor::reactor() {
    m_backend = make_reactor_backend();

    m_on_error = [](const std::exception& e) {
        // Assembled whole before it is written, so two threads reporting at
        // once cannot interleave in the middle of a message.
        const std::string line = std::string("jlib::sys::reactor: ") +
            e.what() + "\n";

        std::cerr << line << std::flush;
    };

    // The wake pipe is a registration like any other, on a token this object
    // keeps to itself and does not count().
    m_wake_token = token(m_next_token++);

    std::shared_ptr<entry> e(new entry);

    e->id = m_wake_token;
    e->fd = m_wake.get_reader();
    e->events = READ;
    e->handler = [this](token, int, event_type) { drain_wake(); };

    m_entries[m_wake_token] = e;
    m_backend->add(e->fd, e->events, e->id);
}

reactor::~reactor() {}

const char* reactor::backend() const { return m_backend->name(); }

void reactor::on_error(error_handler h) { m_on_error = std::move(h); }

void reactor::report(const std::exception& e) {
    if(m_on_error) m_on_error(e);
}

std::size_t reactor::count() const {
    // Less the wake pipe, which is this object's own business.
    return m_entries.size() - 1;
}

std::size_t reactor::timers() const { return m_timers.size(); }

bool reactor::stopped() const { return m_stop.load(); }

reactor::token reactor::add(int fd, event_type events, ready_handler h) {
    for(const auto& i : m_entries) {
        if(i.second->fd == fd) {
            throw exception("that descriptor is already registered; the poll "
                            "backend could express a second registration and "
                            "the other two could not agree what it meant");
        }
    }

    std::shared_ptr<entry> e(new entry);

    e->id = token(m_next_token++);
    e->fd = fd;
    e->events = events;
    e->handler = std::move(h);

    m_entries[e->id] = e;
    m_backend->add(fd, events, e->id);

    return e->id;
}

reactor::token reactor::once(int fd, event_type events, ready_handler h) {
    const token t = add(fd, events, std::move(h));

    m_entries[t]->once = true;

    return t;
}

void reactor::modify(token t, event_type events) {
    auto i = m_entries.find(t);

    if(i == m_entries.end()) throw exception("no such registration");

    m_backend->modify(i->second->fd, i->second->events, events, t);

    i->second->events = events;
}

void reactor::remove(token t) {
    auto i = m_entries.find(t);

    // Benign: a token names something that happened, and two clean-up paths
    // both reaching for it is ordinary.
    if(i == m_entries.end()) return;

    m_backend->remove(i->second->fd, i->second->events);

    // Erased from the map, but the entry itself survives as long as anything
    // holds the shared_ptr -- which the dispatch loop does across the call, so
    // a callback removing itself does not free the closure it is inside.
    m_entries.erase(i);
}

reactor::timer_token reactor::after(std::chrono::nanoseconds d,
                                    timer_handler h)
{
    std::shared_ptr<timer> t(new timer);

    t->id = timer_token(m_next_timer++);
    t->due = std::chrono::steady_clock::now() + d;
    t->handler = std::move(h);

    m_timers[t->id] = t;
    m_by_due.insert(std::make_pair(t->due, t));

    return t->id;
}

reactor::timer_token reactor::every(std::chrono::nanoseconds d,
                                    timer_handler h)
{
    const timer_token id = after(d, std::move(h));

    m_timers[id]->period = d;

    return id;
}

void reactor::cancel(timer_token t) {
    auto i = m_timers.find(t);

    if(i == m_timers.end()) return;

    for(auto j = m_by_due.lower_bound(i->second->due);
        j != m_by_due.end() && j->first == i->second->due; ++j) {
        if(j->second == i->second) { m_by_due.erase(j); break; }
    }

    m_timers.erase(i);
}

void reactor::post(job j) {
    {
        std::lock_guard<std::mutex> guard(m_jobs_lock);

        m_jobs.push(std::move(j));
    }

    wake();
}

void reactor::wake() {
    // Only on the 0 -> 1 edge: a thousand posts cost one byte, and the pipe --
    // which is non-blocking at both ends -- cannot fill and start throwing.
    if(m_woken.exchange(true)) return;

    try { m_wake.write_int(1); }
    catch(std::exception&) { /* full is fine; one byte is all it takes */ }
}

void reactor::drain_wake() {
    try { for(;;) m_wake.read_int(); }
    catch(std::exception&) { /* would_block: drained */ }
}

void reactor::stop() {
    m_stop.store(true);

    wake();
}

std::chrono::nanoseconds reactor::next_wait(std::chrono::nanoseconds budget) const {
    if(m_by_due.empty()) return budget;

    const auto now = std::chrono::steady_clock::now();
    const auto due = m_by_due.begin()->first;

    const std::chrono::nanoseconds until =
        due <= now ? std::chrono::nanoseconds::zero()
                   : std::chrono::duration_cast<std::chrono::nanoseconds>(due - now);

    if(budget < std::chrono::nanoseconds::zero()) return until;

    return until < budget ? until : budget;
}

void reactor::fire_timers() {
    // One at a time, re-checking the front, because a handler may cancel a
    // timer or add one -- including its own.
    for(;;) {
        if(m_by_due.empty()) break;

        const auto now = std::chrono::steady_clock::now();
        auto front = m_by_due.begin();

        if(front->first > now) break;

        std::shared_ptr<timer> t = front->second;

        m_by_due.erase(front);

        const bool repeating = t->period != std::chrono::nanoseconds::zero();

        if(!repeating) m_timers.erase(t->id);

        try { t->handler(t->id); }
        catch(std::exception& e) { report(e); }
        catch(...) { report(exception("a timer threw something that is not "
                                      "an exception")); }

        // Re-armed from *now* rather than from the deadline, so a handler
        // slower than the period drops ticks instead of accumulating a backlog
        // it can never work off.  And only if it is still there: a repeating
        // timer that cancelled itself is gone from m_timers.
        if(repeating && m_timers.count(t->id)) {
            t->due = std::chrono::steady_clock::now() + t->period;

            m_by_due.insert(std::make_pair(t->due, t));
        }
    }
}

bool reactor::run_one(std::chrono::nanoseconds timeout) {
    if(m_dispatching) {
        throw exception("run_one() from inside a callback; the loop is single "
                        "threaded and a nested pass would dispatch the same "
                        "readiness twice");
    }

    m_dispatching = true;

    struct guard {
        bool& flag;
        ~guard() { flag = false; }
    } g{m_dispatching};

    bool pending = false;

    {
        std::lock_guard<std::mutex> lock(m_jobs_lock);

        pending = !m_jobs.empty();
    }

    // Cleared before the wait, so a post() arriving during it writes another
    // byte and the wait returns at once rather than sleeping on queued work.
    m_woken.store(false);

    std::chrono::nanoseconds wait = next_wait(timeout);

    // Anything already queued means do not wait at all.
    if(pending) wait = std::chrono::nanoseconds::zero();

    m_ready.clear();
    m_backend->wait(m_ready, wait);

    // Taken **after** the wait, not before.  A job posted while this pass was
    // blocked woke it, and swapping beforehand would leave that job sitting
    // until the next pass -- so a caller doing a single run_one() would see it
    // return having done nothing, which is the shape of a lost wakeup even
    // though nothing is lost.
    {
        std::lock_guard<std::mutex> lock(m_jobs_lock);

        while(!m_jobs.empty()) {
            m_running.push_back(std::move(m_jobs.front()));
            m_jobs.pop();
        }
    }

    bool did = false;

    for(std::size_t i = 0; i < m_running.size(); i++) {
        did = true;

        try { m_running[i](); }
        catch(std::exception& e) { report(e); }
        catch(...) { report(exception("a job threw something that is not an "
                                      "exception")); }
    }

    m_running.clear();

    for(std::size_t i = 0; i < m_ready.size(); i++) {
        // Resolved here rather than when the list was built, which is what
        // makes remove()'s guarantee hold *within* a pass: a callback that
        // removed this token earlier in this same loop leaves nothing to find.
        auto at = m_entries.find(m_ready[i].first);

        if(at == m_entries.end()) continue;

        // Copied, so the entry outlives a callback that removes itself.
        std::shared_ptr<entry> e = at->second;

        if(e->once) remove(e->id);

        // The wake pipe is not a dispatch anybody asked for.
        if(e->id != m_wake_token) did = true;

        try { e->handler(e->id, e->fd, m_ready[i].second); }
        catch(std::exception& x) { report(x); }
        catch(...) { report(exception("a callback threw something that is not "
                                      "an exception")); }
    }

    const std::size_t was = m_timers.size();

    fire_timers();

    if(was != 0) did = true;

    return did;
}

void reactor::run() {
    while(!m_stop.load()) {
        // Nothing registered, nothing due, nothing posted and nobody holding a
        // work guard: there is no event that could ever arrive.
        if(count() == 0 && m_timers.empty() && m_work.load() == 0) {
            std::lock_guard<std::mutex> lock(m_jobs_lock);

            if(m_jobs.empty()) return;
        }

        run_one();
    }
}

}
}
