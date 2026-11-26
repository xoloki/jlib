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

#ifndef JLIB_TESTS_SCRIPTED_HH
#define JLIB_TESTS_SCRIPTED_HH

/**
 * @file
 *
 * An OpenAI-compatible endpoint that answers whatever it was told to.
 *
 * ## Why not jserve
 *
 * jserve needs a GGUF, which is #236's bargain, and the point of a client
 * test is the *client*. This answers in the real wire format over a real
 * socket and decides nothing, so what a model would actually have said stays
 * untested by anything here -- and always will be.
 *
 * ## Why it is a header and not two copies
 *
 * It was one copy, inside `app_jcode_run_test`, and #447 needed the same
 * endpoint for juanaco. Two hand-rolled servers speaking the same protocol
 * drift, and the drift shows up as one client passing against an envelope
 * the other never sends -- which is the same failure as a prompt and a parser
 * in different files.
 *
 * ## What it can be told to do
 *
 * Answer, answer in pieces, answer slowly, or **answer never**. The last two
 * are not curiosities. A wedged inference server is what a ten-minute timeout
 * exists for, and it is the only way to find out what a client does on the
 * way out while one request is still in flight. And a stream that arrives
 * *instantly* cannot show that a client renders tokens as they come: the
 * whole reply is on screen in one repaint either way, and the assertion
 * passes for a client that waited for all of it.
 *
 * So `first_delay` stands in for prefill -- head sent, then silence, which is
 * exactly what jserve does and what #451 is about -- and `chunk_delay` stands
 * in for the decode rate.
 */

#include <jlib/ai/openai.hh>
#include <jlib/sys/server.hh>
#include <jlib/util/http.hh>

#include <atomic>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <stdlib.h>
#include <unistd.h>

namespace jlib {
namespace tests {

/** What the scripted endpoint will answer, and what it was asked. */
struct script {
    std::string content;             ///< the model's "reply"

    /**
     * Whether to answer with SSE.  **Decided by the request, not set here.**
     *
     * This was a field a test set, and #448 found what that costs: a juanaco
     * test asked for a stream, the endpoint answered a `chat.completion`
     * object because nothing had switched the field on, and the client
     * rendered nothing. Eleven assertions failed and all of them pointed at
     * the client.
     *
     * **A client test whose server ignores the client's own request can only
     * exercise the path the test author remembered to switch on** -- and the
     * one it answers instead is the path that was already working. So it is
     * read out of the request now, the way a real server reads it, and left
     * here only for a test that wants to see what the far end was asked.
     */
    bool stream = false;

    /**
     * Seconds of silence after the response head, before the first event.
     *
     * The head goes out immediately -- a server has to send it before it can
     * stream a body -- so this is the stretch where a client has parsed the
     * status, is inside the body read, and **its sink has not been called
     * once**. Everything about prefill follows from that.
     */
    double first_delay = 0;

    /** Seconds between events afterwards. */
    double chunk_delay = 0;

    /**
     * What `GET /v1/models` lists (#450).
     *
     * This answered *every* request with a chat completion, including a model
     * listing -- which a client that never asked for one could not notice,
     * and juanaco, which asks at startup, would have read as a broken server.
     * Same rule as `stream` above: answer the question that was asked.
     */
    std::vector<ai::openai::listing> models =
        std::vector<ai::openai::listing>(1, ai::openai::listing{ "m", 4096 });

    /** Whether the last chat request asked for `include_usage`. */
    bool include_usage = false;

    /**
     * Read the request and answer nothing, until `closing`.
     *
     * **Bounded rather than forever, and that is not timidity.**
     * `sys::server::stop()` does not interrupt a handler that is already
     * running, so a handler sleeping without end would hold the test's own
     * join at the end of main and turn a client assertion into a hung suite.
     */
    bool hang = false;

    std::atomic<int> requests{0};
    std::string last_body;

    /** Set before stop(), so a hanging handler lets go. */
    std::atomic<bool> closing{false};
};

/** Sleep, in slices, so `closing` can cut it short.  See script::hang. */
inline void nap(const script& sc, double seconds) {
    for(int i = 0; i < int(seconds * 20) && !sc.closing.load(); i++)
        ::usleep(50 * 1000);
}

inline void serve(sys::socketstream& s, script& sc) {
    for(;;) {
        util::http::Request q;

        try { q = util::http::read_request_head(s); }
        catch(std::exception&) { return; }

        const std::string body =
            util::http::read_body(s, q.body_framing(), q.content_length());

        // A model listing is not a completion, and is not counted as one: the
        // tests that count requests are counting what the model was asked.
        if(q.method() == "GET" &&
           q.target().find("/models") != std::string::npos)
        {
            const std::string list = ai::openai::models(sc.models, 0);

            std::ostringstream out;

            out << "HTTP/1.1 200 OK\r\n"
                << "Content-Type: application/json\r\n"
                << "Content-Length: " << list.size() << "\r\n"
                << "Connection: keep-alive\r\n\r\n" << list;

            s << out.str() << std::flush;

            continue;
        }

        sc.requests++;
        sc.last_body = body;

        // The request decides.  Lenient about a body that will not parse:
        // one of these tests sends deliberate rubbish one day, and a server
        // that threw here would look like a client defect.
        try {
            const ai::openai::request asked = ai::openai::request::parse(body);

            sc.stream = asked.stream;
            sc.include_usage = asked.include_usage;
        }
        catch(std::exception&) { sc.stream = false; sc.include_usage = false; }

        if(sc.hang) {
            for(int i = 0; i < 300 && !sc.closing.load(); i++)
                ::usleep(100 * 1000);

            return;
        }

        std::string payload;
        std::string type;

        if(sc.stream) {
            type = "text/event-stream";

            // **One event per element, not one string.** They have to be
            // writable separately for the delays below to mean anything, and
            // the total length still has to be known before the head goes
            // out, so they are built first and sent afterwards.
            std::vector<std::string> events;

            ai::openai::delta first;

            first.role = true;

            events.push_back(
                ai::openai::event(ai::openai::chunk("id", "m", 1, first)));

            // In pieces, because that is what a stream is and the reader has
            // to put them back together.
            for(std::size_t at = 0; at < sc.content.size(); at += 7) {
                ai::openai::delta d;

                d.content = sc.content.substr(at, 7);

                events.push_back(
                    ai::openai::event(ai::openai::chunk("id", "m", 1, d)));
            }

            ai::openai::delta last;

            last.done = true;

            events.push_back(
                ai::openai::event(ai::openai::chunk("id", "m", 1, last)));

            // Only when asked, as jserve does -- see request::include_usage
            // for why a client that did not ask must not be sent one. The
            // counts are the same plausible ones the whole reply carries.
            if(sc.include_usage)
                events.push_back(ai::openai::event(ai::openai::usage(
                    "id", "m", 1, unsigned(double(body.size()) / 3.8), 20)));

            events.push_back(ai::openai::done());

            // **Chunked, not Content-Length, and that distinction cost an
            // afternoon.** This sent an SSE body with a Content-Length, and
            // every delay above was invisible: the client read the whole
            // body and called its sink **once, at the end**, because a
            // reader given a length is entitled to fill its buffer before
            // handing anything over. Nothing was wrong with the client and
            // nothing was wrong with the delays; the response was simply not
            // shaped like a stream.
            //
            // jserve sends chunked SSE, which is what makes incremental
            // delivery the only thing a reader can do -- a chunk is a unit
            // the framing itself delimits. So this sends chunked too, which
            // is the same rule as reading `stream` out of the request: a
            // scripted endpoint that does not behave like the real one tests
            // the client against a server that does not exist.
            std::ostringstream head;

            head << "HTTP/1.1 200 OK\r\n"
                 << "Content-Type: " << type << "\r\n"
                 << "Transfer-Encoding: chunked\r\n"
                 << "Connection: keep-alive\r\n\r\n";

            s << head.str() << std::flush;

            nap(sc, sc.first_delay);

            for(std::size_t i = 0; i < events.size(); i++) {
                // A client that gave up closed the socket, and writing to it
                // is how this finds out.  jlib suppresses SIGPIPE itself --
                // SO_NOSIGPIPE, or a per-thread block where there is none --
                // so the stream merely goes bad.
                if(!s) return;

                s << std::hex << events[i].size() << std::dec << "\r\n"
                  << events[i] << "\r\n" << std::flush;

                if(i + 1 < events.size()) nap(sc, sc.chunk_delay);
            }

            if(s) s << "0\r\n\r\n" << std::flush;

            continue;
        }

        {
            type = "application/json";

            // A plausible count rather than a round number: a client prints
            // its estimate against this, and a made-up 40 would make the
            // drift line read like a defect in the estimator.
            payload = ai::openai::completion("id", "m", 1, sc.content,
                                             ai::openai::finish::stop,
                                             unsigned(double(body.size()) / 3.8),
                                             20);
        }

        std::ostringstream out;

        out << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: " << type << "\r\n"
            << "Content-Length: " << payload.size() << "\r\n"
            << "Connection: keep-alive\r\n\r\n" << payload;

        s << out.str() << std::flush;
    }
}

/** A reply in jcode's whole-file format: a name, a fence, the file, a fence. */
inline std::string listing(const std::string& name, const std::string& body) {
    return "Here you go.\n\n" + name + "\n```\n" + body + "```\n";
}

/** A directory that removes itself, and the files a test puts in it. */
struct tree {
    std::string path;

    tree() {
        char t[] = "/tmp/jlib_scripted_XXXXXX";

        path = ::mkdtemp(t) ? t : "";
    }

    ~tree() {
        if(!path.empty() && system(("rm -rf '" + path + "'").c_str())) {}
    }

    tree(const tree&) = delete;
    tree& operator=(const tree&) = delete;

    void put(const std::string& name, const std::string& body) const {
        std::ofstream o(path + "/" + name, std::ios::binary);

        o << body;
    }

    std::string get(const std::string& name) const {
        std::ifstream i(path + "/" + name, std::ios::binary);

        std::ostringstream all;

        all << i.rdbuf();

        return all.str();
    }
};

}
}

#endif // JLIB_TESTS_SCRIPTED_HH
