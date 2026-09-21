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

/**
 * jcode -- a coding harness over jserve.
 *
 * The loop #242 was about: read the files, lay out a request that fits, send
 * it down a connection that stays open, stream the reply, read the edits out
 * of it, and put them on disk after saying what will happen.
 *
 * Every part of that was built and tested separately -- the wire format both
 * ways (#260), a body that arrives as it is read (#261), a connection that
 * carries several requests (#264), the edit format and containment (#266),
 * the layout and its two guesses (#271). **This is the first time any of them
 * have met each other, and the first time any of them have met a model.**
 *
 * ## What it asks before it writes
 *
 * Every edit, unless --yes. A harness that writes without asking is fine when
 * a version control system is watching and reckless when one is not, and
 * jcode cannot tell which it is in. --dry-run decides everything and writes
 * nothing.
 *
 * ## Loopback, and why the URL is a flag rather than a default to reach past
 *
 * It talks to jserve, which says in its own header that it is for loopback.
 * Nothing here authenticates, and an OpenAI-compatible endpoint on the far
 * side of a network is somebody's paid API with a key this does not carry.
 */

#include <jlib/apps/jcode.hh>

#include <jlib/ai/openai.hh>

#include <memory>
#include <jlib/net/http.hh>
#include <jlib/util/URL.hh>
#include <jlib/util/util.hh>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace ai = jlib::ai;
namespace oa = jlib::ai::openai;
namespace http = jlib::net::http;
namespace jcode = jlib::apps::jcode;
namespace util = jlib::util;

namespace {

struct options {
    std::string url = "http://127.0.0.1:8080/v1";
    std::string model;
    std::string root = ".";

    /**
     * The context to lay out against, in tokens.
     *
     * **Told, not asked.** Nothing in the OpenAI protocol says how big a
     * model's context is -- jserve knows, because it is in the GGUF, and
     * `/v1/models` answers names and nothing else. Extending it is worth
     * doing and is not this program's to do.
     */
    std::size_t context = 8192;

    unsigned int tokens = 0;        ///< max_tokens; 0 leaves it to the server
    bool has_temp = false;
    float temp = 0;

    /**
     * Seconds to wait for a reply, which is not thirty.
     *
     * `net::http::options` defaults to 30 because "a mail client is supposed
     * to sit on a quiet IMAP connection and an HTTP client is not". That is
     * right for a token exchange and wrong here: **an inference server is
     * quiet precisely while it is working**, and jserve's own header says a
     * generation takes as long as it takes and deliberately bounds nothing.
     *
     * Measured: the first request against a freshly loaded 7B model failed at
     * **30.050 seconds** -- the default, to the millisecond -- because a cold
     * model compiles its kernels before it answers anything. The second
     * request took 8.4 seconds and worked.
     *
     * Ten minutes, and a flag. Not zero, which waits forever: a wedged server
     * would hang jcode with nothing to say, and there is no user watching a
     * pipe.
     */
    double timeout = 600;

    bool stream = true;
    /**
     * How many times round the tool loop before stopping.
     *
     * A model that calls the same tool forever is the ordinary failure of a
     * loop like this rather than an exotic one, so there is a bound and
     * reaching it is said out loud. Nothing can be called yet, so today every
     * run takes exactly one round.
     */
    unsigned int rounds = 8;

    /**
     * The build command the model may run, split on whitespace.
     *
     * **Empty by default, and that is the point.** A jcode given no --build
     * has no way to run anything at all; naming one is how a user says which
     * command, rather than jcode inferring it from what it finds lying around.
     * No shell is involved -- see jcode::toolbox.
     */
    std::string build;

    bool dry_run = false;
    bool yes = false;
};

void usage(std::ostream& o, const char* me) {
    o << "usage: " << me << " [options] <request> [file ...]\n"
      << "\n"
      << "  Asks a model to change the files, and applies what comes back.\n"
      << "  Every edit is confirmed unless --yes.\n"
      << "\n"
      << "  --url URL       an OpenAI-compatible endpoint\n"
      << "                  (default http://127.0.0.1:8080/v1)\n"
      << "  --model NAME    which model, when the endpoint serves several\n"
      << "  --root DIR      what the filenames are relative to, and the\n"
      << "                  furthest anything will be written (default .)\n"
      << "  --context N     tokens to lay the request out against\n"
      << "                  (default 8192; the protocol cannot say)\n"
      << "  --tokens N      most tokens in the reply\n"
      << "  --temp F        0 is greedy\n"
      << "  --timeout S     seconds to wait for a reply (default 600; a\n"
      << "                  cold model can take a minute to say anything)\n"
      << "  --no-stream     wait for the whole reply rather than watching it\n"
      << "  --build CMD     a command the model may run, split on\n"
      << "                  spaces and run without a shell; also run\n"
      << "                  after writing, to say whether it still\n"
      << "                  builds; without this it cannot run anything\n"
      << "  --rounds N      most times round the tool loop\n"
      << "                  (default 8; nothing calls tools yet)\n"
      << "  --dry-run       decide everything, write nothing\n"
      << "  --yes           do not ask before writing\n"
      << "  --help\n";
}

bool parse_args(int argc, char** argv, options& o, std::string& request,
                std::vector<std::string>& files)
{
    for(int i = 1; i < argc; i++) {
        const std::string a = argv[i];

        const bool has_next = i + 1 < argc;

        if(a == "--help") { usage(std::cout, argv[0]); return false; }
        else if(a == "--url" && has_next) o.url = argv[++i];
        else if(a == "--model" && has_next) o.model = argv[++i];
        else if(a == "--root" && has_next) o.root = argv[++i];
        else if(a == "--context" && has_next)
            o.context = std::size_t(std::atol(argv[++i]));
        else if(a == "--tokens" && has_next)
            o.tokens = unsigned(std::atoi(argv[++i]));
        else if(a == "--temp" && has_next) {
            o.has_temp = true;
            o.temp = float(std::atof(argv[++i]));
        }
        else if(a == "--timeout" && has_next) o.timeout = std::atof(argv[++i]);
        else if(a == "--no-stream") o.stream = false;
        else if(a == "--build" && has_next) o.build = argv[++i];
        else if(a == "--rounds" && has_next) {
            o.rounds = unsigned(std::atoi(argv[++i]));

            // Zero rounds would ask nothing at all, which is not a thing
            // anyone means by it.
            if(!o.rounds) o.rounds = 1;
        }
        else if(a == "--dry-run") o.dry_run = true;
        else if(a == "--yes") o.yes = true;
        else if(!a.empty() && a[0] == '-') {
            std::cerr << "jcode: no such option: " << a << "\n";

            usage(std::cerr, argv[0]);

            return false;
        }
        else if(request.empty()) request = a;
        else files.push_back(a);
    }

    if(request.empty()) {
        usage(std::cerr, argv[0]);

        return false;
    }

    return true;
}

bool read_file(const std::string& root, const std::string& name,
               jcode::source& into)
{
    std::ifstream in(root + "/" + name, std::ios::binary);

    if(!in) return false;

    std::ostringstream all;

    all << in.rdbuf();

    into.name = name;
    into.content = all.str();

    return true;
}

/** Ask, unless told not to.  An answer that is not yes is no. */
bool confirm(const std::string& what) {
    std::cerr << what << " [y/N] " << std::flush;

    std::string line;

    if(!std::getline(std::cin, line)) return false;

    const std::string said = util::trim(line);

    return said == "y" || said == "Y" || said == "yes";
}

}

int main(int argc, char** argv) {
    options o;

    std::string request;
    std::vector<std::string> names;

    if(!parse_args(argc, argv, o, request, names)) return 1;

    std::vector<jcode::source> files;

    for(const std::string& n : names) {
        jcode::source s;

        if(!read_file(o.root, n, s)) {
            std::cerr << "jcode: cannot read " << o.root << "/" << n << "\n";

            return 1;
        }

        files.push_back(s);
    }

    const std::vector<jcode::tool> tools = jcode::toolbox(o.root, o.build);

    std::cerr << "jcode: the model may";

    for(std::size_t i = 0; i < tools.size(); i++)
        std::cerr << (i ? ", " : " ") << tools[i].name;

    std::cerr << "\n";

    const jcode::plan plan = jcode::lay_out(request, files, o.context, tools);

    for(const std::string& d : plan.dropped)
        std::cerr << "jcode: left out " << d << "\n";

    std::cerr << "jcode: " << plan.sent.size() << " of " << files.size()
              << " file(s), about " << plan.estimate << " tokens of "
              << o.context << "\n";

    // **One exchange, whichever transport the options ask for.**
    //
    // Handed to jcode::converse rather than called directly, so the loop that
    // drives it can be tested with no server at all (#328). The connection is
    // made once, outside: a harness that opens one per call is #221, and a
    // loop would do it once a round.
    std::size_t prompt_tokens = 0;

    bool first = true;

    std::string text;

    const util::URL base(o.url);

    http::options net;

    net.timeout = o.timeout;

    // A whole file of source, and the protocol's envelope around it.  The
    // 1 MiB default is for a token endpoint's reply.
    net.max_body = 32 << 20;

    const util::URL target(o.url + "/chat/completions");

    std::unique_ptr<http::connection> conn;

    const jcode::exchange once =
        [&](const std::vector<ai::message>& turns,
            const std::string& tools) -> oa::answer
    {
        oa::request req;

        req.model = o.model;
        req.stream = o.stream;
        req.max_tokens = o.tokens;
        req.has_temperature = o.has_temp;
        req.temperature = o.temp;
        req.tools = tools;
        req.messages = turns;

        if(!conn) conn.reset(new http::connection(base, net));

        util::http::fields send;

        send.add("Content-Type", "application/json");

        oa::answer a;

        if(o.stream) {
            // The whole chain, first time in one place: the connection reads
            // the body as it arrives, the reader unframes the events, and the
            // deltas concatenate to the reply.
            oa::event_reader events;

            std::string got;

            const util::http::Response r = conn->request(
                "POST", target, send, req.str(),
                [&](std::string_view piece) {
                    for(const std::string& one :
                            events.feed(std::string(piece)))
                    {
                        try {
                            const oa::delta d = oa::delta::parse(one);

                            got += d.content;

                            // Whole calls, because jserve sends them whole --
                            // see openai::delta::calls.
                            for(std::size_t i = 0; i < d.calls.size(); i++)
                                a.calls.push_back(d.calls[i]);

                            std::cout << d.content << std::flush;
                        }
                        catch(std::exception& e) {
                            std::cerr << "\njcode: a chunk that did not parse: "
                                      << e.what() << "\n";
                        }
                    }

                    return true;
                });

            std::cout << "\n";

            // Thrown rather than returned: converse turns it into an ending
            // that says the far end refused, which is a different outcome
            // from the model having nothing to say.
            if(!r.ok()) {
                oa::failure f;

                throw std::runtime_error(
                    "the server refused: " +
                    (oa::failure::parse(r.body(), f) ? f.message : r.reason()));
            }

            if(!events.done())
                std::cerr << "jcode: the stream ended without [DONE], so the "
                          << "reply may be short\n";

            a.content = got;
        }
        else {
            const util::http::Response r =
                conn->request("POST", target, send, req.str());

            if(!r.ok()) {
                oa::failure f;

                throw std::runtime_error(
                    "the server refused: " +
                    (oa::failure::parse(r.body(), f) ? f.message : r.reason()));
            }

            a = oa::answer::parse(r.body());

            std::cout << a.content << "\n";
        }

        // The first reply is the one the estimate was made against.
        if(first) { prompt_tokens = a.prompt_tokens; first = false; }

        return a;
    };

    std::vector<ai::message> opening;

    for(const std::pair<std::string, std::string>& t : plan.turns)
        opening.push_back({ t.first, t.second });

    // Bounded by the same root the edits are written under, and carrying a
    // build tool only if one was named.  See jcode::toolbox for what each one
    // may do and what stops it.

    const jcode::conversation talk =
        jcode::converse(opening, tools, once, o.rounds);

    for(const jcode::ran& c : talk.calls)
        std::cerr << "jcode: called " << c.name
                  << (c.arguments.empty() ? std::string()
                                          : " with " + c.arguments)
                  << (c.known ? (c.failed.empty() ? "" : " -- failed: ")
                              : " -- no such tool")
                  << c.failed << "\n";

    if(talk.why != jcode::ending::answered) {
        std::cerr << "jcode: " << jcode::spell(talk.why)
                  << (talk.detail.empty() ? std::string() : ": " + talk.detail)
                  << "\n";

        return 1;
    }

    text = talk.text;

    // What the guess was worth, said out loud.  The streaming path gets no
    // usage block -- the protocol does not put one in the chunks -- so this
    // is only ever available with --no-stream, and saying nothing is better
    // than implying the estimate was checked.
    const std::string drift = jcode::estimate_drift(plan.estimate,
                                                    prompt_tokens);

    if(!drift.empty()) std::cerr << "jcode: " << drift << "\n";

    const jcode::reply parsed = jcode::parse(text, plan.sent);

    for(const std::string& g : parsed.guesses())
        std::cerr << "jcode: guessed: " << g << "\n";

    // **Said rather than swallowed.**  jcode declares no tools, so a model
    // asking to call one has gone somewhere it was not sent -- but a reply
    // that is entirely a call would otherwise print "no files in that reply",
    // which is what a model too weak to follow the format looks like. They
    // are different problems and silence cannot tell them apart. #316.
    for(const oa::call& c : parsed.calls)
        std::cerr << "jcode: the model asked to call " << c.name
                  << (c.arguments.empty() ? std::string()
                                          : " with " + c.arguments)
                  << ", which jcode cannot do yet\n";

    if(parsed.edits.empty() && parsed.refusals.empty()) {
        std::cerr << "jcode: no files in that reply\n";

        // Not distinguished in the exit status: jcode documents none beyond
        // 0 and 1, and inventing one here would be a contract nobody could
        // find. When #310 gives jcode tools it can actually call, that
        // question has a real case behind it.
        return 0;
    }

    // Decided first, written second: a caller is asked about an edit that is
    // already known to be applicable, rather than being asked and then told it
    // was refused.
    const std::vector<jcode::result> dry = jcode::apply(parsed, o.root, true);

    jcode::reply going;

    for(const jcode::result& r : dry) {
        if(r.what == jcode::outcome::refused) {
            std::cerr << "jcode: refused " << r.name << ": " << r.why << "\n";

            continue;
        }

        if(r.what == jcode::outcome::unchanged) {
            std::cerr << "jcode: " << r.name
                      << " is already what the model sent\n";

            continue;
        }

        if(o.dry_run) {
            // "would written" is what spell() gives, and it is not English.
            // The outcomes are named for what happened, which is the right
            // name everywhere except in front of "would".
            std::cerr << "jcode: would "
                      << (r.what == jcode::outcome::created ? "create" : "write")
                      << " " << r.name << "\n";

            continue;
        }

        if(!o.yes && !confirm(std::string("jcode: ") +
                              (r.what == jcode::outcome::created ? "create"
                                                                 : "write") +
                              " " + r.name + "?"))
        {
            std::cerr << "jcode: left " << r.name << " alone\n";

            continue;
        }

        for(const jcode::edit& e : parsed.edits)
            if(e.name == r.name) going.edits.push_back(e);
    }

    if(going.edits.empty()) return 0;

    bool wrote = false;

    for(const jcode::result& r : jcode::apply(going, o.root)) {
        std::cerr << "jcode: " << jcode::spell(r.what) << " " << r.name
                  << (r.why.empty() ? "" : ": " + r.why) << "\n";

        if(r.what == jcode::outcome::written ||
           r.what == jcode::outcome::created) wrote = true;
    }

    // **"written" is not the same as "and it still works".**
    //
    // Measured: a model asked to fix three functions returned a file that
    // kept all three bugs and called three functions it had not defined, and
    // jcode said "written util.c" and exited 0. The tree no longer compiled.
    // Announcing a write and not what the write did is the harness lie this
    // arc keeps finding in different clothes.
    //
    // Only when a build command was named -- the same one the model may call,
    // and the one #242 requires be named rather than inferred. Nothing is
    // reverted: undoing a write the user confirmed is a larger decision than
    // this, and a build that fails is information rather than grounds to
    // discard their work.
    if(wrote && !o.build.empty()) {
        const std::vector<jcode::tool> check = jcode::toolbox(o.root, o.build);

        for(const jcode::tool& t : check) {
            if(t.name != "build") continue;

            const std::string said = t.run("{}");

            // Named, not paraphrased. `--build 'make check'` runs the
            // tests as well as the compiler, so "it still builds" would be
            // a claim about something this did not measure -- a correct fix
            // that leaves one test failing is not a broken build.
            if(said.compare(0, 7, "exit 0\n") == 0) {
                std::cerr << "jcode: and `" << o.build << "` passes\n";

                break;
            }

            std::cerr << "jcode: but `" << o.build << "` now fails:\n";

            // The first lines only: the point is that it broke and roughly
            // where, and the whole log is what --build is for.
            std::istringstream lines(said);
            std::string line;

            for(int i = 0; i < 12 && std::getline(lines, line); i++)
                std::cerr << "  " << line << "\n";

            return 1;
        }
    }

    return 0;
}
