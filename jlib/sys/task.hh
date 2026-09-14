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
#ifndef JLIB_SYS_TASK_HH
#define JLIB_SYS_TASK_HH

#include <coroutine>
#include <exception>
#include <utility>

namespace jlib {
namespace sys {

/**
 * A coroutine that produces a T, once.
 *
 * The return type for anything that suspends: an I/O read, a protocol exchange,
 * anything built out of those.  jlib owns this because the standard has no
 * task type -- C++26's std::execution is the first, and `<execution>` on both
 * toolchains here is still the parallel-algorithms header with no
 * __cpp_lib_senders.  See #4, which records that as out of scope rather than
 * undecided.
 *
 * ## Lazy
 *
 * `task<T> t = f();` does not run f.  Nothing happens until the task is
 * awaited, or start()ed by a driver.  Eager would run f on whichever thread
 * constructed the task, which for an I/O task is the wrong thread by
 * definition -- the reactor's thread is the one that may touch its
 * descriptors.
 *
 * ## Where it runs
 *
 * Wherever resume() is called, and nowhere else.  A coroutine frame is heap
 * memory with no thread affinity, so a task suspended on one thread and
 * resumed on another *moves*.  That is a feature and a hazard: it is how a
 * pool hop is expressed, and it is how OpenSSL's thread-local error queue gets
 * read on a different thread from the one that cleared it.
 *
 * jlib's answer is that the reactor is single-threaded, so nothing migrates
 * unless a caller writes a `co_await` that says to.  Every migration point is
 * visible in the source.  #4 records why that was chosen over a reactor
 * several threads pump.
 *
 * ## Two things that are easy to get wrong
 *
 * **final_suspend returns the waiter's handle rather than resuming it.**  That
 * is symmetric transfer, and it compiles to a tail call.  Calling
 * `waiter.resume()` from within final_suspend instead grows the real stack by
 * a frame per await, so a long chain of reads overflows it -- which is a crash
 * under load rather than a compile error, and is why there is a test that
 * awaits several thousand deep.
 *
 * **coroutine_handle<Derived> does not convert to coroutine_handle<Base>.**
 * There is no relationship between the handle types even though there is
 * between the promises, so final_awaiter::await_suspend has to be a template.
 */
template<typename T> class task;

namespace detail {

/** What every task promise carries, whatever it returns. */
struct task_promise_base {
    std::suspend_always initial_suspend() noexcept { return {}; }

    struct final_awaiter {
        bool await_ready() noexcept { return false; }

        // Templated on the promise: see the note above about handle
        // conversion.  Returning the handle rather than resuming it is the
        // symmetric transfer that keeps a deep chain off the real stack.
        template<typename P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept
        {
            std::coroutine_handle<> next = h.promise().m_waiter;

            // noop_coroutine when nobody is waiting -- a task driven by
            // run_until_complete rather than awaited.  Returning a null handle
            // would be undefined.
            return next ? next : std::noop_coroutine();
        }

        void await_resume() noexcept {}
    };

    final_awaiter final_suspend() noexcept { return {}; }

    void unhandled_exception() { m_error = std::current_exception(); }

    std::coroutine_handle<> m_waiter;
    std::exception_ptr      m_error;

    // Whether the initial suspend has been left.  start() is idempotent
    // because of it, and that is not tidiness: resuming a coroutine that is
    // parked at a co_await continues it *as though the await had completed*,
    // which finishes the task and leaves the reactor holding a registration
    // whose handle is about to be destroyed.  run_until_complete() calling
    // start() on an already-started task was exactly that, and the test caught
    // it as a registration that would not go away.
    bool m_started = false;
};

}

template<typename T>
class task {
public:
    struct promise_type : detail::task_promise_base {
        task get_return_object() {
            return task(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        template<typename U>
        void return_value(U&& v) { m_value = std::forward<U>(v); }

        T m_value{};
    };

    typedef std::coroutine_handle<promise_type> handle_type;

    explicit task(handle_type h) : m_handle(h) {}

    ~task() { if(m_handle) m_handle.destroy(); }

    task(task&& o) noexcept : m_handle(std::exchange(o.m_handle, handle_type())) {}

    task& operator=(task&& o) noexcept {
        if(this != &o) {
            if(m_handle) m_handle.destroy();

            m_handle = std::exchange(o.m_handle, handle_type());
        }

        return *this;
    }

    task(const task&) = delete;
    task& operator=(const task&) = delete;

    bool valid() const { return bool(m_handle); }

    bool done() const { return m_handle && m_handle.done(); }

    /**
     * Start it without awaiting it.
     *
     * For a driver -- run_until_complete -- rather than for a coroutine, which
     * should co_await instead so that the two are chained.
     */
    /**
     * Run it to its first suspension.  Idempotent.
     *
     * Starting an already-started task must not resume it -- see m_started.
     */
    void start() {
        if(!m_handle || m_handle.done() || m_handle.promise().m_started) return;

        m_handle.promise().m_started = true;

        m_handle.resume();
    }

    /**
     * The value, or the exception the coroutine let escape.
     *
     * @throws whatever the coroutine threw
     */
    T result() {
        if(m_handle.promise().m_error)
            std::rethrow_exception(m_handle.promise().m_error);

        return std::move(m_handle.promise().m_value);
    }

    auto operator co_await() {
        struct awaiter {
            handle_type m_task;

            bool await_ready() { return m_task.done(); }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<> w) {
                m_task.promise().m_waiter = w;
                m_task.promise().m_started = true;

                // Symmetric transfer into the task, so awaiting is a tail call
                // in both directions.
                return m_task;
            }

            T await_resume() {
                if(m_task.promise().m_error)
                    std::rethrow_exception(m_task.promise().m_error);

                return std::move(m_task.promise().m_value);
            }
        };

        return awaiter{m_handle};
    }

private:
    handle_type m_handle;
};

/** The void case, which everything that reads for effect rather than value wants. */
template<>
class task<void> {
public:
    struct promise_type : detail::task_promise_base {
        task get_return_object() {
            return task(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        void return_void() {}
    };

    typedef std::coroutine_handle<promise_type> handle_type;

    explicit task(handle_type h) : m_handle(h) {}

    ~task() { if(m_handle) m_handle.destroy(); }

    task(task&& o) noexcept : m_handle(std::exchange(o.m_handle, handle_type())) {}

    task& operator=(task&& o) noexcept {
        if(this != &o) {
            if(m_handle) m_handle.destroy();

            m_handle = std::exchange(o.m_handle, handle_type());
        }

        return *this;
    }

    task(const task&) = delete;
    task& operator=(const task&) = delete;

    bool valid() const { return bool(m_handle); }

    bool done() const { return m_handle && m_handle.done(); }

    /**
     * Run it to its first suspension.  Idempotent.
     *
     * Starting an already-started task must not resume it -- see m_started.
     */
    void start() {
        if(!m_handle || m_handle.done() || m_handle.promise().m_started) return;

        m_handle.promise().m_started = true;

        m_handle.resume();
    }

    void result() {
        if(m_handle.promise().m_error)
            std::rethrow_exception(m_handle.promise().m_error);
    }

    auto operator co_await() {
        struct awaiter {
            handle_type m_task;

            bool await_ready() { return m_task.done(); }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<> w) {
                m_task.promise().m_waiter = w;
                m_task.promise().m_started = true;

                return m_task;
            }

            void await_resume() {
                if(m_task.promise().m_error)
                    std::rethrow_exception(m_task.promise().m_error);
            }
        };

        return awaiter{m_handle};
    }

private:
    handle_type m_handle;
};

}
}

#endif // JLIB_SYS_TASK_HH
