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
 * Answer, answer in pieces, or **answer never**. The last one is not a
 * curiosity: a wedged inference server is what a ten-minute timeout exists
 * for, and it is the only way to find out what a client does on the way out
 * while one request is still in flight.
 */

#include <jlib/ai/openai.hh>
#include <jlib/sys/server.hh>
#include <jlib/util/http.hh>

#include <atomic>
#include <fstream>
#include <sstream>
#include <string>

#include <stdlib.h>
#include <unistd.h>

namespace jlib {
namespace tests {

/** What the scripted endpoint will answer, and what it was asked. */
struct script {
    std::string content;             ///< the model's "reply"
    bool stream = false;

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

inline void serve(sys::socketstream& s, script& sc) {
    for(;;) {
        util::http::Request q;

        try { q = util::http::read_request_head(s); }
        catch(std::exception&) { return; }

        const std::string body =
            util::http::read_body(s, q.body_framing(), q.content_length());

        sc.requests++;
        sc.last_body = body;

        if(sc.hang) {
            for(int i = 0; i < 300 && !sc.closing.load(); i++)
                ::usleep(100 * 1000);

            return;
        }

        std::string payload;
        std::string type;

        if(sc.stream) {
            type = "text/event-stream";

            ai::openai::delta first;

            first.role = true;

            payload += ai::openai::event(
                ai::openai::chunk("id", "m", 1, first));

            // In pieces, because that is what a stream is and the reader has
            // to put them back together.
            for(std::size_t at = 0; at < sc.content.size(); at += 7) {
                ai::openai::delta d;

                d.content = sc.content.substr(at, 7);

                payload += ai::openai::event(
                    ai::openai::chunk("id", "m", 1, d));
            }

            ai::openai::delta last;

            last.done = true;

            payload += ai::openai::event(
                ai::openai::chunk("id", "m", 1, last));
            payload += ai::openai::done();
        }
        else {
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
