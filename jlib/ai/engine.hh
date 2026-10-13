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

#ifndef JLIB_AI_ENGINE_HH
#define JLIB_AI_ENGINE_HH

#include <jlib/ai/chat.hh>
#include <jlib/ai/generate.hh>
#include <jlib/ai/gguf.hh>
#include <jlib/ai/model.hh>
#include <jlib/ai/tokenizer.hh>

#include <jlib/sys/reactor.hh>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace jlib {
namespace ai {

/**
 * The models a server has loaded, and who is allowed to use one.
 *
 * A model file is 1-3 GB and takes seconds to read, so a server loads one
 * once and keeps it.  That much is obvious.  What is not is the second half:
 *
 * ## A model instance holds one conversation
 *
 * `model<T>` is not a stateless compute engine.  The key-value cache lives in
 * it -- `block::m_kc`, `block::m_vc`, `m_cache_len` -- along with `m_seq` and
 * the activation scratch.  **That cache is the conversation.**  Two requests
 * running through one instance would interleave their keys and produce
 * fluent nonsense, silently, which is this library's characteristic failure.
 *
 * So a loaded model is a *serialisation point*, not a worker pool.  `acquire`
 * hands out one `session` at a time and the rest wait.  Two different models
 * run concurrently; two conversations against the same model do not.
 *
 * Making them concurrent means separating the weights, which are read-only
 * after `load()`, from the cache and scratch, which are not -- so that N
 * conversations share one copy of a 2.8 GB tensor set.  That is a real
 * refactor of `model` and `block` and it is not this.  See #199.
 *
 * ## The cache is reset when a session is taken, not when it is returned
 *
 * Returning it would be enough if every holder exited cleanly.  A request
 * that dies mid-generation does not, and the next conversation would inherit
 * its keys -- again silently.  Resetting on acquire does not depend on the
 * previous holder having behaved.
 *
 * It costs nothing to reset: `reset_cache()` sets a length to zero and frees
 * no memory.  What it forecloses is reusing a cached prefix across requests,
 * which nothing here does -- an OpenAI-shaped request carries the whole
 * conversation every time and is re-prefilled from the start regardless.
 */
template<typename T>
class engine {
private:
    // Ahead of everything, because `session` is public and refers to it: a
    // member declaration is not a complete-class context, so a nested class
    // cannot name a type the enclosing one declares later.
    struct entry {
        /**
         * Whether the model is loaded, askable **without the lock**.
         *
         * prepare() needs to distinguish "not loaded yet" from "loaded and
         * busy", and the lock cannot tell it: a caller holding it might be
         * loading or might be generating.  Reading `m` instead would be a
         * data race.
         *
         * Written under the lock, once, after the load; read anywhere.
         */
        std::atomic<bool> ready{false};

        std::string name;
        std::string path;

        // Guards everything below, and is held for a whole conversation.
        std::mutex lock;

        std::unique_ptr<tokenizer> tok;
        std::unique_ptr<chat> ch;
        std::unique_ptr<model<T> > m;

        unsigned int context = 0;

        /**
         * When this model was last free, in steady-clock seconds; 0 if it is
         * not loaded.
         *
         * **Atomic and outside the lock on purpose.** A sweep has to decide
         * whether an entry is worth trying *before* it tries, and taking the
         * lock to read a timestamp would mean waiting on the very conversation
         * it is deciding about -- which is the thing the sweep must never do.
         *
         * Stamped in two places, and it needs both: after a load, so a model
         * that `prepare()` loaded and nobody ever used still ages out, and in
         * `session`'s destructor, so one that has been used ages from when it
         * was last released rather than from when it was first loaded.
         */
        std::atomic<long long> freed{0};

        /** Now, in the units `freed` is kept in. */
        static long long stamp() {
            return std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }
    };

public:
    class exception : public std::exception {
    public:
        exception(const std::string& msg)
            : m_msg("jlib::ai::engine: " + msg) {}

        const char* what() const throw() { return m_msg.c_str(); }

    private:
        std::string m_msg;
    };

    /**
     * @param b the backend every model here runs on, which must outlive this
     *
     * A reference rather than a copy because `model` holds one too.  One
     * backend serves every model: on Metal it carries a single command
     * stream, so GPU work is serialised across models whatever this class
     * does about locking -- which is worth knowing before concluding that two
     * models run in parallel.  They interleave.
     */
    explicit engine(backend<T>& b) : m_b(b) {}

    /**
     * Name a model file without reading it.
     *
     * Loading is deferred to the first `acquire`, so a server can advertise
     * models it has not paid for yet -- which is what `/v1/models` wants, and
     * what starting in under a second wants.
     */
    void add(const std::string& name, const std::string& path);

    /** The names, in the order they were added. */
    std::vector<std::string> names() const;

    /** Whether a name has been added; says nothing about loading. */
    bool has(const std::string& name) const;

    /** Whether it is in memory now. */
    bool loaded(const std::string& name) const;

    /**
     * Exclusive use of one model, for the length of one conversation.
     *
     * Blocks until the model is free.  Loads it first if this is the first
     * ask, which can take seconds -- and which happens while holding the
     * model's own lock, so a second caller waits for the load rather than
     * starting a duplicate one.
     */
    class session {
    public:
        ~session();

        session(session&&);
        session& operator=(session&&) = delete;

        session(const session&) = delete;
        session& operator=(const session&) = delete;

        typedef ai::model<T> model_type;

        model_type& model() const { return *m_m->m; }
        const tokenizer& tok() const { return *m_m->tok; }
        const chat& templ() const { return *m_m->ch; }

        /** What the file says it can hold, or what the engine capped it to. */
        unsigned int context() const { return m_m->context; }

        /**
         * Run a generation on this session's model.
         *
         * A forwarder to ai::generate, and worth having for two reasons.  A
         * session already knows which model it holds, so pairing one with
         * somebody else's is a mistake that cannot be made through here --
         * the caller used to reach in for model() and supply a backend
         * alongside it.  And it is the seam a test drives: `endpoint` is
         * templated on the engine, so a fake session answers this with a
         * fixed sequence of tokens and the whole HTTP path runs with no model
         * file (#236).
         *
         * The backend is still the caller's, because the engine does not own
         * one -- it is passed to the constructor and shared by every model.
         *
         * `still_wanted` is asked between prefill chunks, where `on_token`
         * cannot be because prefill produces none -- see ai::generate, and
         * #283 for the held model that is.
         */
        std::vector<int> generate(backend<T>& b, const std::vector<int>& prompt,
                                  unsigned int max_new, sampler& s,
                                  const stops& ends = stops(),
                                  std::function<bool(int)> on_token = nullptr,
                                  std::function<bool()> still_wanted = nullptr)
        {
            return ai::generate<T>(model(), b, prompt, max_new, s, ends,
                                   on_token, still_wanted);
        }

        const std::string& name() const { return m_m->name; }

    private:
        friend class engine;

        session(entry* e, std::unique_lock<std::mutex> lock);

        entry* m_m;
        std::unique_lock<std::mutex> m_lock;
    };

    session acquire(const std::string& name);

    /**
     * A session, or nothing at all, without ever blocking.
     *
     * For a caller on a thread that must not block -- a reactor, where
     * blocking stops every other connection it is carrying and, if the lock
     * is held by a generation that needs that same reactor to finish,
     * permanently (#273).
     *
     * **It does not load.** `acquire()` takes the lock before the load so that
     * a second caller waits rather than starting a duplicate, which means the
     * first acquisition of a model reads gigabytes with the lock held. That is
     * fine on a thread that may block and disastrous on one that may not, so
     * this refuses a model that is not loaded yet rather than quietly becoming
     * the blocking version. Load it first -- see load().
     */
    std::optional<session> try_acquire(const std::string& name);

    /**
     * Make sure a model is loaded, here, now, blocking.
     *
     * The other half of not blocking a reactor, and the half `try_acquire()`
     * cannot do: `acquire()` loads **inside the lock**, so the first
     * acquisition of a model reads gigabytes with it held. `try_acquire()`
     * would succeed on that first call -- the lock is free -- and then do the
     * load wherever it was called from.
     *
     * So a caller that must not block calls this first, from somewhere that
     * may: a pool thread. It takes the lock and releases it **on that one
     * thread**, which matters, because a `std::mutex` must be unlocked by the
     * thread that locked it and a session carries its lock with it.
     *
     * Two cold callers still do not load twice: the lock inside serialises
     * them, and the second waits where waiting is allowed.
     */
    void prepare(const std::string& name);

    /**
     * How long a model may sit unused before a sweep may drop it; 0 is never,
     * and is the default (#278).
     *
     * **Idleness, not pressure.** Reclaiming on idleness cannot thrash: a model
     * two conversations are alternating between is never idle, so it is never a
     * candidate, and the alternating case stays as honest as it is today. A
     * policy that evicted to make room would reload gigabytes on every turn
     * instead -- measured at 2.7s for a 7.5 GB model -- which is worse than the
     * single clear failure it replaces.
     *
     * Zero by default because a server given one model on loopback is the
     * common case and should see none of this.
     */
    void keep_alive(unsigned int seconds) { m_keep_alive = seconds; }

    unsigned int keep_alive() const { return m_keep_alive; }

    /**
     * Drop every loaded model that nothing has wanted for `keep_alive()`
     * seconds, and say how many went.
     *
     * **The caller chooses the thread, and it must not be a reactor's.**
     * Freeing gigabytes is not instant and this takes a model's lock while it
     * happens, so run it from a pool thread or a thread of its own (#273's
     * rule, and the same reason `try_acquire()` does not load).
     *
     * A model in use is skipped, not waited for: the lock is taken with
     * `try_to_lock`, and a held lock *is* a conversation in progress. So a
     * sweep never drops a model out from under a caller and never blocks
     * behind one.
     *
     * Returns 0 and does nothing when `keep_alive()` is 0.
     */
    unsigned int sweep();

    /**
     * Drop one model now, whether or not it is idle. Returns whether it had
     * been loaded.
     *
     * Blocks until the model is free, so it waits for a conversation rather
     * than skipping it -- the opposite of `sweep()`, because a caller naming
     * one model has asked for that model and not for whatever is convenient.
     * Refuses to run on a reactor thread for the same reason `acquire()` does.
     */
    bool unload(const std::string& name);

    /** Which models are loaded right now, in `add()` order. */
    std::vector<std::string> resident() const;

private:
    entry* find(const std::string& name) const;

    /** Everything, so a sweep can walk it without holding the vector's lock. */
    std::vector<std::shared_ptr<entry> > snapshot() const;

    backend<T>& m_b;

    /** See keep_alive(); 0 is never, which is the default. */
    unsigned int m_keep_alive = 0;

    // Held by pointer so a session can keep one across a rehash, and because
    // entry is not movable -- it has a mutex in it.
    std::vector<std::shared_ptr<entry> > m_models;

    // Guards the vector, not the models.  Held only long enough to find one.
    mutable std::mutex m_lock;
};

template<typename T>
void engine<T>::add(const std::string& name, const std::string& path) {
    std::lock_guard<std::mutex> guard(m_lock);

    for(const std::shared_ptr<entry>& e : m_models)
        if(e->name == name)
            throw exception("a model called \"" + name + "\" is already here");

    std::shared_ptr<entry> e(new entry);

    e->name = name;
    e->path = path;

    m_models.push_back(e);
}

template<typename T>
std::vector<std::string> engine<T>::names() const {
    std::lock_guard<std::mutex> guard(m_lock);

    std::vector<std::string> out;

    for(const std::shared_ptr<entry>& e : m_models) out.push_back(e->name);

    return out;
}

template<typename T>
typename engine<T>::entry* engine<T>::find(const std::string& name) const {
    std::lock_guard<std::mutex> guard(m_lock);

    for(const std::shared_ptr<entry>& e : m_models)
        if(e->name == name) return e.get();

    return 0;
}

template<typename T>
bool engine<T>::has(const std::string& name) const {
    return find(name) != 0;
}

template<typename T>
bool engine<T>::loaded(const std::string& name) const {
    entry* e = find(name);

    if(!e) return false;

    // Without the model's own lock: this is a question about the past by the
    // time it is answered, and taking the lock would make asking it wait for
    // a whole conversation.
    return bool(e->m);
}

template<typename T>
typename engine<T>::session engine<T>::acquire(const std::string& name) {
    // **This blocks, and a reactor thread may not.**  Not "should not": the
    // lock is held for the length of a generation, and if the holder needs
    // this reactor to report its tokens back then waiting here is a deadlock
    // with no way out (#273).
    //
    // It fires whether or not the lock is contended, which is the point: the
    // contended case needs two clients at once and this needs only one.
    if(sys::on_a_reactor_thread())
        throw exception("acquire() blocks, and this is a reactor thread; "
                        "use try_acquire() and answer 503 when it is busy");

    entry* e = find(name);

    if(!e) throw exception("no model called \"" + name + "\"");

    // Taken before the load, so a second caller arriving during a first
    // caller's load waits for it rather than starting a duplicate.  A load is
    // seconds and gigabytes; two at once is the one case worth this much
    // holding.
    std::unique_lock<std::mutex> lock(e->lock);

    if(!e->m) {
        const gguf g(e->path);

        // Read out of the file rather than holding it: neither of these keeps
        // a reference, so the file closes at the end of this scope and a
        // loaded model owes nothing to the path it came from.
        e->tok.reset(new tokenizer(g));
        e->ch.reset(new chat(g, e->tok->token(e->tok->eos())));

        const typename model<T>::config c = model<T>::config::from(g);

        e->m.reset(new model<T>(m_b, c));
        e->m->load(g);
        e->m->enable_cache();

        e->context = c.context;
    }

    // Under the lock, so nothing can see it true before the model is there.
    e->ready.store(true);

    // So a model loaded and then never used still ages out.  The destructor
    // stamps it again on release; this covers the gap before the first one.
    e->freed.store(entry::stamp());

    return session(e, std::move(lock));
}

template<typename T>
std::optional<typename engine<T>::session>
engine<T>::try_acquire(const std::string& name) {
    entry* e = find(name);

    if(!e) throw exception("no model called \"" + name + "\"");

    std::unique_lock<std::mutex> lock(e->lock, std::try_to_lock);

    if(!lock.owns_lock()) return std::nullopt;

    // Not loaded is not the same as busy, and neither is a thing to do here:
    // loading reads the file with the lock held, which is exactly the block
    // this exists to avoid.  A caller that reaches this has not called load().
    if(!e->m)
        throw exception("the model \"" + name + "\" is not loaded, and "
                        "try_acquire() will not load it; call prepare() from "
                        "a thread that may block");

    return session(e, std::move(lock));
}

template<typename T>
void engine<T>::prepare(const std::string& name) {
    // It blocks by contract, so it belongs under the same rule as acquire()
    // -- and this catches the caller who hops in the wrong direction, or
    // forgets to hop at all.
    if(sys::on_a_reactor_thread())
        throw exception("prepare() loads the model and blocks, and this is a "
                        "reactor thread; hop to a pool thread first");

    entry* e = find(name);

    if(!e) throw exception("no model called \"" + name + "\"");

    // **Already loaded is the common case and must not touch the lock.**
    //
    // The first version of this was `session s = acquire(name);`, which loads
    // correctly and then waits for whoever holds the model -- so a second
    // request to a busy model queued on a pool thread for the length of a
    // generation and then succeeded, instead of being told the model was
    // busy.  Measured: both of two overlapping requests returned 200, the
    // second 6.3 seconds late, and the 503 never fired.
    //
    // A load happens once; being busy happens constantly.  This returns
    // immediately for the second and only blocks for the first.
    if(e->ready.load()) return;

    // Not loaded.  Blocking here is the point -- a second cold caller waits
    // for the first rather than starting its own load of the same gigabytes,
    // which is the ordering acquire() has always had.  Taken and dropped on
    // this thread, so no lock crosses one.
    session s = acquire(name);

    (void)s;
}

template<typename T>
engine<T>::session::session(entry* e, std::unique_lock<std::mutex> lock)
    : m_m(e), m_lock(std::move(lock))
{
    // On the way in, not on the way out: a previous holder that died
    // mid-generation did not reset anything, and inheriting its keys would be
    // a wrong answer rather than an error.  See the note at the top.
    m_m->m->reset_cache();
}

template<typename T>
engine<T>::session::session(session&& o)
    : m_m(o.m_m), m_lock(std::move(o.m_lock))
{
    o.m_m = 0;
}

template<typename T>
engine<T>::session::~session() {
    // **With the lock still held.** A destructor's body runs before its
    // members are destroyed, and `m_lock` is a member, so the entry is still
    // this thread's while the stamp is written -- which is what keeps a sweep
    // from reading a half-released entry.
    //
    // Guarded because a moved-from session has no entry: the move constructor
    // nulls it, and both halves of a move run this.
    if(m_m) m_m->freed.store(entry::stamp());
}

template<typename T>
std::vector<std::shared_ptr<typename engine<T>::entry> >
engine<T>::snapshot() const {
    std::lock_guard<std::mutex> guard(m_lock);

    return m_models;
}

template<typename T>
unsigned int engine<T>::sweep() {
    if(!m_keep_alive) return 0;

    // A copy first, so the vector's lock is not held while gigabytes are
    // freed -- `add()` and `names()` would wait behind it otherwise.
    const std::vector<std::shared_ptr<entry> > all = snapshot();

    const long long now = entry::stamp();

    unsigned int dropped = 0;

    for(const std::shared_ptr<entry>& e : all) {
        // Askable without the lock, which is why `ready` is atomic.
        if(!e->ready.load()) continue;

        const long long when = e->freed.load();

        if(!when || now - when < static_cast<long long>(m_keep_alive)) continue;

        // **try_to_lock, never lock.** A held lock is a conversation in
        // progress; waiting for it would make a sweep block on exactly the
        // work it exists to stay out of the way of.
        std::unique_lock<std::mutex> lock(e->lock, std::try_to_lock);

        if(!lock) continue;

        // Everything above was read without the lock, so read it again with
        // it: a conversation could have begun and ended in between, which
        // moves the stamp and makes this entry no longer a candidate.
        if(!e->m) continue;

        const long long again = e->freed.load();

        if(!again || now - again < static_cast<long long>(m_keep_alive)) continue;

        // False before the model goes, under the lock, so nothing sees it
        // true with no model behind it -- the mirror of acquire().
        e->ready.store(false);

        e->m.reset();
        e->ch.reset();
        e->tok.reset();

        e->context = 0;
        e->freed.store(0);

        dropped++;
    }

    return dropped;
}

template<typename T>
bool engine<T>::unload(const std::string& name) {
    // Frees gigabytes with a lock held and waits for a conversation to end.
    // Both are reasons a reactor thread may not be here; see acquire().
    if(sys::on_a_reactor_thread())
        throw exception("unload() blocks, and this is a reactor thread");

    entry* e = find(name);

    if(!e) throw exception("no model called \"" + name + "\"");

    // Blocking, unlike sweep(): a caller naming one model has asked for that
    // model, not for whichever one happens to be free.
    std::lock_guard<std::mutex> lock(e->lock);

    if(!e->m) return false;

    e->ready.store(false);

    e->m.reset();
    e->ch.reset();
    e->tok.reset();

    e->context = 0;
    e->freed.store(0);

    return true;
}

template<typename T>
std::vector<std::string> engine<T>::resident() const {
    const std::vector<std::shared_ptr<entry> > all = snapshot();

    std::vector<std::string> out;

    for(const std::shared_ptr<entry>& e : all)
        if(e->ready.load()) out.push_back(e->name);

    return out;
}

}
}

#endif // JLIB_AI_ENGINE_HH
