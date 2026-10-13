/* -*- mode: C++ c-basic-offset: 4 -*-
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

#include <jlib/ai/engine.hh>

#ifdef HAVE_METAL
#include <jlib/metal/backend.hh>
#endif

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace ai = jlib::ai;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static std::string find_named(const std::string& name) {
    const std::string where[] = { "", "../", "../../", "../../../" };

    for(const std::string& w : where) {
        std::ifstream f(w + name, std::ios::binary);

        if(f) return w + name;
    }

    return "";
}

/** Naming a model must not read it: a server advertises more than it holds. */
static void naming_a_model_does_not_load_it() {
    std::cout << "\nnaming a model does not load it:\n";

    ai::host_backend<float> b;
    ai::engine<float> e(b);

    e.add("first", "/nonexistent/a.gguf");
    e.add("second", "/nonexistent/b.gguf");

    ok("  both names are there", e.has("first") && e.has("second"));

    ok("  in the order they were added",
       e.names() == std::vector<std::string>({ "first", "second" }));

    // The paths do not exist, so this passing at all is the assertion: add()
    // has not opened anything.
    ok("  and neither is loaded", !e.loaded("first") && !e.loaded("second"));

    ok("  a name nobody added is not there", !e.has("third"));

    bool threw = false;

    try { e.acquire("third"); }
    catch(ai::engine<float>::exception&) { threw = true; }

    ok("  and acquiring it is refused", threw);

    threw = false;

    try { e.add("first", "/nonexistent/c.gguf"); }
    catch(ai::engine<float>::exception&) { threw = true; }

    ok("  a name cannot be added twice", threw);
}

/**
 * The bookkeeping half of keep-alive, which needs no model (#278).
 */
static void keep_alive_is_off_until_it_is_asked_for() {
    std::cout << "\nkeep-alive is off until it is asked for:\n";

    ai::host_backend<float> b;
    ai::engine<float> e(b);

    e.add("first", "/nonexistent/a.gguf");

    ok("  no idle timeout by default", e.keep_alive() == 0,
       std::to_string(e.keep_alive()));

    // The reason it is the default: a server given one model on loopback is
    // the common case and should behave exactly as it did before this existed.
    ok("  and a sweep does nothing at all with none set", e.sweep() == 0);

    e.keep_alive(30);

    ok("  it can be set", e.keep_alive() == 30, std::to_string(e.keep_alive()));

    // Still nothing: the paths do not exist, so nothing is loaded, so there is
    // nothing for a sweep to drop. Asserted because a sweep that touched an
    // unloaded entry would have to open the file to know that.
    ok("  a sweep with nothing loaded still drops nothing", e.sweep() == 0);

    ok("  and nothing is resident", e.resident().empty(),
       std::to_string(e.resident().size()));

    ok("  unloading what was never loaded says so", !e.unload("first"));

    bool threw = false;

    try { e.unload("third"); }
    catch(ai::engine<float>::exception&) { threw = true; }

    ok("  unloading a name nobody added is refused", threw);
}

/**
 * An idle model is dropped, a busy one is not (#278).
 *
 * `ai::engine` loaded a model on the first `acquire` and never released it:
 * no unload, no eviction, no timeout. That is the missing half of a decision
 * already taken -- deferring the load is what lets a daemon advertise more
 * models than it can hold, and never unloading is what makes that
 * advertisement a lie the moment a second one is asked for.
 *
 * **Reclaim on idleness, never on pressure**, which is why this test is about
 * a timer and not about memory. Measured on this machine: a load larger than
 * free physical memory *succeeds* and pages rather than failing, so there is
 * no allocation error to catch and recover from; and a 7.5 GB model costs
 * ~2.7s to reload, so evicting under pressure would pay that on every turn of
 * an alternating conversation. A timeout cannot thrash, because a model two
 * conversations are alternating between is never idle.
 *
 * The two assertions that carry it are **that a busy model is skipped** and
 * **that an idle one goes**. The first is the one with teeth: a sweep takes
 * each lock with `try_to_lock`, so a conversation in progress is passed over
 * rather than waited for, and a model is never dropped out from under a
 * caller.
 */
static void an_idle_model_is_dropped_and_a_busy_one_is_not(const std::string& path) {
    std::cout << "\nan idle model is dropped, a busy one is not:\n";

    ai::host_backend<float> b;
    ai::engine<float> e(b);

    e.add("tiny", path);

    {
        ai::engine<float>::session s = e.acquire("tiny");

        ok("  acquiring loads it", e.loaded("tiny"));
    }

    ok("  and it stays loaded after the session ends", e.loaded("tiny"));

    ok("  resident names it", e.resident() == std::vector<std::string>({ "tiny" }));

    // Long enough that nothing is idle yet, so this separates "swept because
    // idle" from "swept because a sweep drops whatever it finds".
    e.keep_alive(3600);

    ok("  a sweep leaves a model that is not idle yet", e.sweep() == 0 &&
       e.loaded("tiny"));

    // **Busy, so skipped.** The stamp is already old enough below, so the only
    // thing keeping this model is the held lock.
    e.keep_alive(1);

    {
        ai::engine<float>::session s = e.acquire("tiny");

        std::this_thread::sleep_for(std::chrono::milliseconds(2100));

        ok("  a sweep skips a model with a conversation on it",
           e.sweep() == 0 && e.loaded("tiny"));
    }

    // Released, and the stamp is from the release rather than from the load,
    // so it has to age again.
    ok("  which is still loaded once the conversation ends", e.loaded("tiny"));

    // **This is what the second stamp is for.** That conversation was held for
    // longer than the keep-alive, so a model aging from its *load* would be
    // over the limit the instant it was released and would go on the very next
    // sweep -- dropped for being idle at the moment it stopped being used.
    // Releasing resets the clock, so this sweep finds nothing.
    ok("  and a sweep straight after release leaves it alone",
       e.sweep() == 0 && e.loaded("tiny"));

    std::this_thread::sleep_for(std::chrono::milliseconds(2100));

    ok("  and once idle it goes", e.sweep() == 1);

    ok("  leaving nothing resident", !e.loaded("tiny") && e.resident().empty());

    // The point of all of it: the name is still advertised, so a request can
    // still have it -- dropping a model is not forgetting it.
    ok("  but the model is still named", e.has("tiny") &&
       e.names() == std::vector<std::string>({ "tiny" }));

    {
        ai::engine<float>::session s = e.acquire("tiny");

        ok("  and asking again loads it back", e.loaded("tiny"));
    }

    ok("  unloading on demand reports that it did", e.unload("tiny"));

    ok("  and a second unload reports that there was nothing to do",
       !e.unload("tiny"));

    ok("  with nothing resident either way", e.resident().empty());
}

/**
 * One session at a time, per model.
 *
 * The property the whole class exists for: `model<T>` holds the key-value
 * cache, so two conversations through one instance would interleave their
 * keys.  Asserted by showing a second acquire does not return while a first
 * is outstanding, and does the moment it is released.
 */
static void a_model_serves_one_conversation_at_a_time(const std::string& path) {
    std::cout << "\na model serves one conversation at a time:\n";

#ifdef HAVE_METAL
    jlib::metal::backend<_Float16> b;
    ai::engine<_Float16> e(b);
#else
    ai::host_backend<float> b;
    ai::engine<float> e(b);
#endif

    e.add("m", path);

    std::atomic<bool> second_in(false);
    std::atomic<bool> second_out(false);

    std::thread t;

    {
        auto first = e.acquire("m");

        ok("  the first acquire loaded it", e.loaded("m"));

        t = std::thread([&]{
            second_in = true;

            // Released immediately; what is being tested is when it returns.
            auto second = e.acquire("m");

            second_out = true;
        });

        // Long enough that a second acquire would have returned if it could.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        ok("  a second caller has asked", second_in.load());

        ok("  and is still waiting while the first holds it",
           !second_out.load());
    }

    // Joined rather than polled: the second acquire returns when the first is
    // released, and if it does not this hangs, which is a clearer failure
    // than a flag that never flips.
    t.join();

    ok("  it gets in once the first is released", second_out.load());
}

/**
 * The cache is reset when a session is taken.
 *
 * Not when it is returned: a request that dies mid-generation returns
 * nothing, and the next conversation would inherit its keys.  Resetting on
 * the way in does not depend on the previous holder having behaved -- so this
 * abandons a session *without* generating cleanly, and checks the next one
 * starts empty regardless.
 */
static void a_session_starts_with_an_empty_cache(const std::string& path) {
    std::cout << "\na session starts with an empty cache:\n";

#ifdef HAVE_METAL
    jlib::metal::backend<_Float16> b;
    ai::engine<_Float16> e(b);
#else
    ai::host_backend<float> b;
    ai::engine<float> e(b);
#endif

    e.add("m", path);

    unsigned int after = 0;

    {
        auto s = e.acquire("m");

        ok("  it starts empty", s.model().cached() == 0,
           std::to_string(s.model().cached()));

        // Put something in the cache, the way a reply would.
        const std::vector<int> ids = s.tok().encode("The capital of France is");

        s.model().reserve(unsigned(ids.size()));

        typename std::remove_reference<decltype(b)>::type::tensor_ptr logits =
            b.make(s.model().conf().vocab, unsigned(ids.size()));

        s.model().forward(ids, logits);
        b.wait();

        after = s.model().cached();

        ok("  a forward pass fills it", after > 0, std::to_string(after));
    }

    {
        auto s = e.acquire("m");

        ok("  and the next session finds it empty again",
           s.model().cached() == 0, std::to_string(s.model().cached()));
    }
}

/** What a session carries besides the model. */
static void a_session_carries_what_a_reply_needs(const std::string& path) {
    std::cout << "\na session carries what a reply needs:\n";

#ifdef HAVE_METAL
    jlib::metal::backend<_Float16> b;
    ai::engine<_Float16> e(b);
#else
    ai::host_backend<float> b;
    ai::engine<float> e(b);
#endif

    e.add("m", path);

    auto s = e.acquire("m");

    ok("  the name it was asked for", s.name() == "m", s.name());

    ok("  a tokenizer that reads the file's vocabulary",
       s.tok().size() > 0, std::to_string(s.tok().size()));

    ok("  a chat template", !s.templ().tmpl().empty());

    ok("  and the context the file states",
       s.context() > 0 && s.context() == s.model().conf().context,
       std::to_string(s.context()));
}

int main() {
    std::cout << std::unitbuf;

    naming_a_model_does_not_load_it();
    keep_alive_is_off_until_it_is_asked_for();

    const std::string path = find_named("tinyllama-1.1b-chat-v1.0.Q8_0.gguf");

    if(path.empty()) {
        std::cout << "\n  (no model file, so only the bookkeeping is "
                  << "exercised)\n";
    }
    else {
        a_model_serves_one_conversation_at_a_time(path);
        a_session_starts_with_an_empty_cache(path);
        a_session_carries_what_a_reply_needs(path);
        an_idle_model_is_dropped_and_a_busy_one_is_not(path);
    }

    // What a green run does not establish.
    //
    // That two models run concurrently in any useful sense.  They do not
    // serialise against each other here, and on Metal they share one command
    // stream, so their GPU work interleaves whatever this class does about
    // locking.  Nothing measures that.
    //
    // Nothing about **pressure**.  There is an idle timeout now (#278), and a
    // model nothing has wanted for long enough is dropped -- but admission is
    // untouched: ask for two models that do not both fit and the second load
    // still goes ahead, succeeds, and pages, because a load that does not fit
    // does not fail.  Measured, not assumed.  A resident cap is the other half
    // and is not here.
    //
    // And nothing about a session outliving the engine, which is a
    // use-after-free this does not prevent -- a session holds a raw entry
    // pointer and a lock on a mutex the engine owns.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
