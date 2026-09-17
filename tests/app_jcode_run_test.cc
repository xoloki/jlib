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
 */

// jcode, end to end, with no model.
//
// The pieces of #242 all have their own tests and none of them had ever met:
// the wire format (#260), a body read as it arrives (#261), a connection
// (#264), the edit format (#266), the layout (#271). This runs the program
// against a scripted OpenAI endpoint and asserts what lands on disk.
//
// The server is hand-rolled rather than jserve because jserve needs a GGUF --
// that is #236's bargain, and the point here is the *client*. What a real
// model says is still untested by anything, and always will be by this.

#include <jlib/ai/openai.hh>
#include <jlib/sys/server.hh>
#include <jlib/sys/sys.hh>
#include <jlib/util/http.hh>

#include <atomic>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <stdlib.h>
#include <unistd.h>

namespace oa = jlib::ai::openai;
namespace sys = jlib::sys;
namespace util = jlib::util;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** The program, beside this test in the build tree.  Not $PATH; see #243. */
static std::string find_jcode() {
    const char* names[] = { "../jlib/apps/jcode", "./jlib/apps/jcode",
                            "../../build/jlib/apps/jcode" };

    for(const char* n : names) {
        std::ifstream f(n);

        if(f) return n;
    }

    return "";
}

struct tree {
    std::string path;

    tree() {
        char t[] = "/tmp/jcode_run_XXXXXX";

        path = ::mkdtemp(t) ? t : "";
    }

    ~tree() {
        if(!path.empty() && system(("rm -rf '" + path + "'").c_str())) {}
    }

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

/** What the scripted endpoint will answer, and what it was asked. */
struct script {
    std::string content;             ///< the model's "reply"
    bool stream = false;
    std::atomic<int> requests{0};
    std::string last_body;
};

static void serve(sys::socketstream& s, script& sc) {
    for(;;) {
        util::http::Request q;

        try { q = util::http::read_request_head(s); }
        catch(std::exception&) { return; }

        const std::string body =
            util::http::read_body(s, q.body_framing(), q.content_length());

        sc.requests++;
        sc.last_body = body;

        std::string payload;
        std::string type;

        if(sc.stream) {
            type = "text/event-stream";

            oa::delta first;

            first.role = true;

            payload += oa::event(oa::chunk("id", "m", 1, first));

            // In pieces, because that is what a stream is and the reader has
            // to put them back together.
            for(std::size_t at = 0; at < sc.content.size(); at += 7) {
                oa::delta d;

                d.content = sc.content.substr(at, 7);

                payload += oa::event(oa::chunk("id", "m", 1, d));
            }

            oa::delta last;

            last.done = true;

            payload += oa::event(oa::chunk("id", "m", 1, last));
            payload += oa::done();
        }
        else {
            type = "application/json";

            // A plausible count rather than a round number: jcode prints
            // its estimate against this, and a made-up 40 would make the
            // drift line read like a defect in the estimator.
            payload = oa::completion("id", "m", 1, sc.content,
                                     oa::finish::stop,
                                     unsigned(double(body.size()) / 3.8), 20);
        }

        std::ostringstream out;

        out << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: " << type << "\r\n"
            << "Content-Length: " << payload.size() << "\r\n"
            << "Connection: keep-alive\r\n\r\n" << payload;

        s << out.str() << std::flush;
    }
}

struct run_result {
    int status = 0;
    std::string out;
    std::string err;
};

static run_result run_jcode(const std::string& jcode, const std::string& url,
                            const tree& t, const std::vector<std::string>& more)
{
    run_result r;

    std::vector<std::string> argv{ jcode, "--url", url, "--root", t.path,
                                   "--no-stream", "--yes" };

    for(const std::string& m : more) argv.push_back(m);

    try { r.status = sys::run(argv, r.out, r.err); }
    catch(std::exception& e) { r.err = e.what(); r.status = -1; }

    return r;
}

static std::string listing(const std::string& name, const std::string& body) {
    return "Here you go.\n\n" + name + "\n```\n" + body + "```\n";
}

int main() {
    std::cout << std::unitbuf;

    const std::string jcode = find_jcode();

    if(jcode.empty()) {
        std::cerr << "no jcode built here\n";

        return 77;
    }

    std::cout << "app_jcode_run_test: " << jcode << "\n";

    script sc;

    sys::server server(0,
                       [&sc](sys::socketstream& s, const sys::peer&) {
                           serve(s, sc);
                       },
                       "127.0.0.1");

    server.on_error([](const std::exception&, const sys::peer&) {});

    std::thread th([&server]{ server.run(); });

    const std::string url = "http://127.0.0.1:" +
                            std::to_string(server.port()) + "/v1";

    {
        std::cout << "\nan edit, all the way through:\n";

        tree t;

        t.put("hello.cc", "int main() { return 1; }\n");

        sc.content = listing("hello.cc", "int main() { return 0; }\n");

        const run_result r = run_jcode(jcode, url, t,
                                       { "make it succeed", "hello.cc" });

        ok("jcode succeeds", r.status == 0,
           std::to_string(r.status) + " " + r.err);

        ok("  the file is what the model said",
           t.get("hello.cc") == "int main() { return 0; }\n", t.get("hello.cc"));

        ok("  and it says what it did",
           r.err.find("written hello.cc") != std::string::npos, r.err);

        // The request really carried the file and the system prompt.
        ok("  the request carried the file",
           sc.last_body.find("int main() { return 1; }") != std::string::npos);

        ok("  and the format instructions",
           sc.last_body.find("three backticks") != std::string::npos);

        // The estimate is checked against what the server counted, which is
        // the only place the true ratio is ever visible from this side.
        ok("  and it reports what the guess was worth",
           r.err.find("estimated") != std::string::npos &&
           r.err.find("the server counted") != std::string::npos, r.err);
    }

    {
        std::cout << "\na reply that stopped mid-file:\n";

        tree t;

        t.put("hello.cc", "original\n");

        // No closing fence: the context wall, or a dropped connection.
        sc.content = "hello.cc\n```\nhalf a file and then not";

        const run_result r = run_jcode(jcode, url, t, { "change it", "hello.cc" });

        ok("nothing is written", t.get("hello.cc") == "original\n",
           t.get("hello.cc"));

        ok("  and it says why",
           r.err.find("ended inside") != std::string::npos, r.err);
    }

    {
        std::cout << "\nan edit to somewhere else entirely:\n";

        tree t;

        t.put("hello.cc", "original\n");

        const std::string away = "/tmp/jcode_escape_" +
                                 t.path.substr(t.path.find_last_of('/') + 1) + ".cc";

        ::unlink(away.c_str());

        sc.content = listing(away, "escaped\n");

        const run_result r = run_jcode(jcode, url, t, { "escape", "hello.cc" });

        ok("is refused", r.err.find("refused") != std::string::npos, r.err);

        ok("  and nothing is written there",
           ::access(away.c_str(), F_OK) != 0, away);

        ::unlink(away.c_str());
    }

    {
        std::cout << "\nwhat --dry-run does:\n";

        tree t;

        t.put("hello.cc", "original\n");

        sc.content = listing("hello.cc", "changed\n");

        const run_result r = run_jcode(jcode, url, t,
                                       { "--dry-run", "change it", "hello.cc" });

        ok("it says what it would do, in English",
           r.err.find("would write hello.cc") != std::string::npos, r.err);

        ok("  and does not do it", t.get("hello.cc") == "original\n");
    }

    {
        std::cout << "\nthe guesses reach the user:\n";

        tree t;

        t.put("hello.cc", "original\n");

        // The envelope a model actually sends: bold, and the prompt's own
        // path prefix pasted back.
        sc.content = listing("**path/to/hello.cc**", "guessed\n");

        const run_result r = run_jcode(jcode, url, t, { "change it", "hello.cc" });

        ok("the edit still lands", t.get("hello.cc") == "guessed\n",
           t.get("hello.cc"));

        ok("  and both guesses are printed",
           r.err.find("guessed:") != std::string::npos &&
           r.err.find("bold") != std::string::npos &&
           r.err.find("basename") != std::string::npos, r.err);
    }

    {
        std::cout << "\nstreaming, which is what the machinery was for:\n";

        tree t;

        t.put("hello.cc", "original\n");

        sc.stream = true;
        sc.content = listing("hello.cc", "streamed\n");

        run_result r;

        std::vector<std::string> argv{ jcode, "--url", url, "--root", t.path,
                                       "--yes", "stream it", "hello.cc" };

        try { r.status = sys::run(argv, r.out, r.err); }
        catch(std::exception& e) { r.err = e.what(); r.status = -1; }

        sc.stream = false;

        ok("the reply arrives in pieces and still parses",
           t.get("hello.cc") == "streamed\n", t.get("hello.cc"));

        ok("  and was printed as it came",
           r.out.find("streamed") != std::string::npos, r.out);
    }

    server.stop();
    th.join();

    // What a green run does not establish.
    //
    // **Nothing about a model.** Every reply here was scripted. What a model
    // actually sends is untested by anything in make check and needs a GGUF,
    // which is #236's bargain one app over.
    //
    // Not the confirm gate: every run passes --yes, because a test cannot
    // answer a prompt. The gate is the default and is exercised by hand.
    //
    // Not a large context, not a real trim, and not usage drift on the
    // streaming path -- the protocol puts no usage block in chunks, so that
    // number is only ever available with --no-stream.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
