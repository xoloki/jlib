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

#include <jlib/apps/jcode.hh>

#include <jlib/util/json.hh>
#include <climits>
#include <jlib/sys/sys.hh>

#include <jlib/util/util.hh>

#include <algorithm>
#include <fstream>
#include <sstream>

#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

namespace jlib {
namespace apps {
namespace jcode {

namespace {

/** Longer than any name a model means, and shorter than a paragraph. */
const std::size_t NAME_LIMIT = 250;

/** Whether a line is a fence: three or more backticks, or three tildes. */
bool fenced(const std::string& line, std::string& info) {
    const std::string s = util::trim(line);

    std::size_t n = 0;

    const char c = s.empty() ? '\0' : s[0];

    if(c != '`' && c != '~') return false;

    while(n < s.size() && s[n] == c) n++;

    if(n < 3) return false;

    // ```cpp on an opening fence; a closing one carries nothing, but models
    // put things there too and it costs nothing to ignore them.
    info = util::trim(s.substr(n));

    return true;
}

std::string basename_of(const std::string& path) {
    const std::string::size_type at = path.find_last_of('/');

    return at == std::string::npos ? path : path.substr(at + 1);
}

/**
 * The name a model meant, and what had to be assumed to get there.
 *
 * Every strip here is a model failure aider recorded first; each one appends
 * to `why` so that the caller can see what was decided on its behalf.
 */
std::string clean(std::string name, const std::vector<std::string>& known,
                  std::vector<std::string>& why)
{
    const std::string raw = name;

    name = util::trim(name);

    // **filename.cc** -- markdown bold, which a model adds when it is being
    // helpful about structure.
    if(name.size() > 2 && name.front() == '*' && name.back() == '*') {
        while(!name.empty() && name.front() == '*') name.erase(0, 1);
        while(!name.empty() && name.back() == '*') name.pop_back();

        why.push_back("the filename was in bold: " + raw);
    }

    // `filename.cc` -- inline code, ditto.
    if(name.size() > 1 && name.front() == '`' && name.back() == '`') {
        while(!name.empty() && name.front() == '`') name.erase(0, 1);
        while(!name.empty() && name.back() == '`') name.pop_back();

        why.push_back("the filename was in backticks: " + raw);
    }

    // # filename.cc -- a heading.
    if(!name.empty() && name.front() == '#') {
        while(!name.empty() && name.front() == '#') name.erase(0, 1);

        name = util::trim(name);

        why.push_back("the filename was a heading: " + raw);
    }

    // filename.cc: -- the model introducing it rather than naming it.
    if(!name.empty() && name.back() == ':') {
        name.pop_back();

        why.push_back("the filename ended in a colon: " + raw);
    }

    name = util::trim(name);

    // **A line with a space in it, before a fence, is prose.**
    //
    // Models narrate between blocks -- "Sorry, better:" was the line that
    // caught this -- and the last line before a fence is then a sentence
    // rather than a name. Taking it produced an edit to a file called
    // "Sorry, better", which is worse than declining: the caller sees a
    // plausible-looking result and the real target is untouched.
    //
    // A filename with a space in it does exist, so an exact match against
    // something that was sent wins over the heuristic. Nothing else does.
    if(name.find_first_of(" \t") != std::string::npos &&
       std::find(known.begin(), known.end(), name) == known.end())
    {
        // The strips that led here described a filename that is not one, so
        // they would only mislead.  The reason it was dropped is the guess
        // worth keeping, and the caller gets that from the fallback below.
        why.clear();

        return std::string();
    }

    // A paragraph is not a filename.  aider draws this line at 250 and the
    // number matters less than having one.
    if(name.size() > NAME_LIMIT) {
        why.push_back("a line of " + std::to_string(name.size()) +
                      " characters before a fence is prose, not a filename");

        return std::string();
    }

    // The prefix out of the prompt's own example.  A model that was shown
    // `path/to/filename.js` will send `path/to/` back, and the file it means
    // is the one whose basename matches.
    if(!name.empty() && !known.empty() &&
       std::find(known.begin(), known.end(), name) == known.end())
    {
        const std::string base = basename_of(name);

        if(base != name &&
           std::find(known.begin(), known.end(), base) != known.end())
        {
            why.push_back("no file called \"" + name + "\" was sent, but \"" +
                          base + "\" was; took the basename");

            name = base;
        }
    }

    return name;
}

}

std::vector<std::string> reply::guesses() const {
    std::vector<std::string> all;

    for(const edit& e : edits)
        for(const std::string& g : e.guesses) all.push_back(e.name + ": " + g);

    return all;
}

// ---------------------------------------------------------------- the prompt

namespace {

/**
 * The worst bytes-per-token ratio measured, which is the safe one.
 *
 * TinyLlama's 32k vocabulary on jlib's densest header (math/matrix.hh) --
 * every other pairing tried was looser, so every other pairing is
 * over-estimated by this, which is the direction that does not lose the head
 * of a conversation.  See the note on estimate_tokens().
 */
const double BYTES_PER_TOKEN = 2.80;

}

double bytes_per_token() { return BYTES_PER_TOKEN; }

std::size_t estimate_tokens(const std::string& text) {
    if(text.empty()) return 0;

    // Rounded up: a fragment of a token is a token.
    return std::size_t(double(text.size()) / BYTES_PER_TOKEN) + 1;
}

std::string system_prompt(const std::vector<tool>& tools) {
    // **Registering a tool is not the same as telling the model it has one.**
    //
    // Measured: with read_file and search registered but unmentioned here, a
    // model asked about a header it had not been given answered "Please
    // provide the contents of util.h" -- which is the rule three lines below
    // doing exactly what it says. The template renders the tool list, and the
    // instructions still won.
    std::string asking;

    if(!tools.empty()) {
        asking = "\nYou can ask for what you need before answering. Rather "
                 "than asking the user for a file or for where something is "
                 "defined, call one of these and then answer with what it "
                 "told you: ";

        for(std::size_t i = 0; i < tools.size(); i++)
            asking += (i ? ", " : "") + tools[i].name;

        // **The markup is named here because the model's own is not stable.**
        //
        // Measured against Qwen2.5-Coder-7B at Q4_K_M: asked with no system
        // turn it answers `<function_call>` inside a fenced xml block; asked
        // with the one above it answers bare JSON and no markers at all. Its
        // template asked for `<tool_call>` in both cases.
        //
        // Bare JSON is the one thing that cannot be accepted by the reader:
        // jcode sends whole files, so a reply containing a .json file would
        // have it read as a call and swallowed out of the content. Naming the
        // markup is the half of the contract this end controls -- the same
        // arrangement as the filename-and-fence rules above, and for the same
        // reason.
        asking += ".\n\nTo call one, answer with exactly this and nothing "
                  "else:\n\n<tool_call>\n{\"name\": \"the tool\", "
                  "\"arguments\": {\"...\": \"...\"}}\n</tool_call>\n";
    }

    // Deliberately close to what aider asks for, because that is the format
    // jserve was verified against end to end and the one models have been
    // trained on the shape of.  What differs is that this says what happens
    // when the rules are broken, since parse() is the other half of it and a
    // model told the consequence has a chance of avoiding it.
    return
        "You are a careful programmer working on an existing codebase.\n"
        "\n"
        "When you change a file you MUST return its entire new contents, in "
        "this format and no other:\n"
        "\n"
        "path/to/filename.cc\n"
        "```\n"
        "the whole file, from its first line to its last\n"
        "```\n"
        "\n"
        "- the filename goes on a line of its own, exactly as it was given to "
        "you: no bold, no backticks, no heading, no trailing colon, and no "
        "path you were not given\n"
        "- the fence is three backticks, alone on its line\n"
        "- never abbreviate with \"...\" or \"rest of file unchanged\": what "
        "you return replaces the file, so anything you leave out is deleted\n"
        "- return a file only if you changed it\n"
        "- if a change is not needed, or the request is unclear, say so in "
        "prose and return no file at all\n" + asking;
}

plan lay_out(const std::string& request, const std::vector<source>& files,
             std::size_t budget, const std::vector<tool>& tools)
{
    plan out;

    const std::string system = system_prompt(tools);

    // The two that are never dropped, costed first: without the system turn
    // the reply is in no format at all, and without the request there is
    // nothing to answer.
    std::size_t used = estimate_tokens(system) + estimate_tokens(request);

    std::string body;

    for(const source& f : files) {
        // A listing costs its content plus the envelope around it, and the
        // envelope is not free on a short file.
        const std::string one = f.name + "\n```\n" + f.content +
                                (f.content.empty() || f.content.back() == '\n'
                                 ? "" : "\n") + "```\n\n";

        const std::size_t cost = estimate_tokens(one);

        if(used + cost > budget) {
            // **Whole files, never truncated.**  Half a file in a prompt is
            // worse than no file: a model completes it rather than noticing,
            // and the reply then rewrites what it never saw.
            out.dropped.push_back(f.name + " (" + std::to_string(cost) +
                                  " tokens; " + std::to_string(used) +
                                  " of " + std::to_string(budget) +
                                  " already used)");

            continue;
        }

        body += one;
        used += cost;

        out.sent.push_back(f.name);
    }

    out.turns.push_back(std::make_pair(std::string("system"), system));

    out.turns.push_back(std::make_pair(std::string("user"),
                                       body.empty()
                                           ? request
                                           : body + request));

    out.estimate = used;

    return out;
}

std::string estimate_drift(std::size_t estimated, std::size_t actual) {
    if(!actual) return std::string();

    const double out_by = double(estimated) - double(actual);

    const double pct = out_by * 100.0 / double(actual);

    std::ostringstream said;

    said << "estimated " << estimated << " prompt tokens, the server counted "
         << actual << " (";

    if(out_by == 0) said << "exact";
    else said << (out_by > 0 ? "over" : "under") << " by "
              << std::size_t(out_by > 0 ? out_by : -out_by) << ", "
              << std::size_t(pct > 0 ? pct + 0.5 : -pct + 0.5) << "%";

    said << ")";

    return said.str();
}

const char* spell(outcome o) {
    switch(o) {
    case outcome::written:   return "written";
    case outcome::unchanged: return "unchanged";
    case outcome::created:   return "created";
    case outcome::refused:   return "refused";
    }

    return "refused";
}

const char* spell(ending e) {
    switch(e) {
    case ending::answered: return "answered";
    case ending::bounded:  return "the round limit was reached";
    case ending::refused:  return "refused";
    case ending::failed:   return "failed";
    }

    return "?";
}

std::string declare(const std::vector<tool>& tools) {
    if(tools.empty()) return std::string();

    util::json::array::ptr out = util::json::array::create();

    for(std::size_t i = 0; i < tools.size(); i++) {
        util::json::object::ptr fn = util::json::object::create();

        fn->add(std::string("name"), tools[i].name);

        if(!tools[i].description.empty())
            fn->add(std::string("description"), tools[i].description);

        // Parsed and re-emitted rather than spliced as text, so a schema that
        // is not JSON is caught here -- where the tool was declared -- rather
        // than by the far end as a malformed request.
        //
        // Safe to hand over: `create` owns what it parses and `add` takes that
        // ownership. The hazard is the other direction, handing `add` an
        // object *borrowed* from something else, which is the double free
        // #311 found in the `functions` wrapper.
        fn->add(std::string("parameters"),
                util::json::object::create(tools[i].parameters.empty()
                                               ? "{\"type\":\"object\"}"
                                               : tools[i].parameters));

        util::json::object::ptr one = util::json::object::create();

        one->add(std::string("type"), std::string("function"));
        one->add(std::string("function"), fn);

        out->add(one);
    }

    return out->str();
}

namespace {

/**
 * A path under `root`, resolved -- or empty if it is not under `root`.
 *
 * The same rule apply() enforces for writes, asked here for reads. Resolution
 * rather than inspection: `..`, an absolute path and a symlink pointing out
 * are one question with one answer, where three string checks would be three
 * chances to disagree.
 */
std::string under(const std::string& root, const std::string& name) {
    char rootbuf[PATH_MAX];

    if(!::realpath(root.c_str(), rootbuf)) return std::string();

    const std::string resolved_root(rootbuf);

    // Resolved whole, not by parent: a read has no reason to accept a name
    // that does not exist, and the file itself is what must be inside.
    const std::string full = name.empty() || name[0] == '/'
                                 ? name : root + "/" + name;

    char buf[PATH_MAX];

    if(!::realpath(full.c_str(), buf)) return std::string();

    const std::string resolved(buf);

    if(resolved == resolved_root) return resolved;

    // The separator matters: /rootlike must not pass for being under /root.
    if(resolved.size() > resolved_root.size() &&
       resolved.compare(0, resolved_root.size(), resolved_root) == 0 &&
       resolved[resolved_root.size()] == '/')
        return resolved;

    return std::string();
}

/** The argument a call gave, or empty. */
std::string argument(const std::string& json, const std::string& key) {
    try {
        util::json::object::ptr o = util::json::object::create(json);

        return o->get(key).str_or(std::string());
    }
    catch(std::exception&) { return std::string(); }
}

/** `text`, cut to `cap` and saying so when it was. */
std::string capped(const std::string& text, std::size_t cap) {
    if(text.size() <= cap) return text;

    // Said in the result rather than left to be noticed. A model shown a
    // silently shortened file believes it read the whole thing, which is a
    // worse answer than a refusal.
    return text.substr(0, cap) + "\n[truncated: " +
           std::to_string(text.size()) + " bytes, showing " +
           std::to_string(cap) + "]";
}

}

std::vector<tool> toolbox(const std::string& root, const std::string& build,
                          std::size_t cap)
{
    std::vector<tool> out;

    {
        tool t;

        t.name = "read_file";
        t.description = "Read a file from the project, by a path relative to "
                        "the project root.";
        t.parameters =
            R"({"type":"object","properties":{"path":{"type":"string",)"
            R"("description":"path relative to the project root"}},)"
            R"("required":["path"]})";

        t.run = [root, cap](const std::string& args) -> std::string {
            const std::string name = argument(args, "path");

            if(name.empty()) return "error: no path given";

            const std::string at = under(root, name);

            // The refusal is the *result*, not an exception: a model that
            // asked for something it may not have should be told so and get
            // another turn, rather than having the conversation end.
            if(at.empty())
                return "error: \"" + name + "\" is not a readable file under "
                       "the project root";

            std::ifstream in(at.c_str(), std::ios::binary);

            if(!in) return "error: cannot read \"" + name + "\"";

            std::ostringstream buf;

            buf << in.rdbuf();

            return capped(buf.str(), cap);
        };

        out.push_back(t);
    }

    {
        tool t;

        t.name = "search";
        t.description = "Find a string in the project's files. Answers with "
                        "path:line:text for each match.";
        t.parameters =
            R"({"type":"object","properties":{"text":{"type":"string",)"
            R"("description":"the string to look for"}},"required":["text"]})";

        t.run = [root, cap](const std::string& args) -> std::string {
            const std::string want = argument(args, "text");

            if(want.empty()) return "error: nothing to search for";

            char rootbuf[PATH_MAX];

            if(!::realpath(root.c_str(), rootbuf))
                return "error: the project root does not resolve";

            // grep through sys::run rather than a shell, and with `--` so a
            // pattern starting with a dash is a pattern rather than a flag.
            std::vector<std::string> argv{ "grep", "-rnI", "--", want,
                                           std::string(rootbuf) };

            std::string got, err;

            try {
                const int status = sys::run(argv, got, err);

                // grep answers 1 for "no matches", which is an answer.
                if(status > 1)
                    return "error: search failed: " +
                           (err.empty() ? std::to_string(status) : err);
            }
            catch(std::exception& e) {
                return std::string("error: could not search: ") + e.what();
            }

            if(got.empty()) return "no matches";

            // Paths come back absolute because grep was given an absolute
            // root; shown relative, which is what the model may ask for.
            const std::string prefix = std::string(rootbuf) + "/";

            std::string shown;

            std::istringstream lines(got);
            std::string line;

            while(std::getline(lines, line)) {
                if(line.compare(0, prefix.size(), prefix) == 0)
                    line = line.substr(prefix.size());

                shown += line + "\n";
            }

            return capped(shown, cap);
        };

        out.push_back(t);
    }

    // **Only when one was named.**  A jcode given no --build has no way to run
    // anything at all, which is the default this arc is under instruction to
    // keep.
    if(!build.empty()) {
        std::vector<std::string> argv;

        {
            std::istringstream words(build);
            std::string one;

            while(words >> one) argv.push_back(one);
        }

        if(!argv.empty()) {
            tool t;

            t.name = "build";
            t.description = "Run the project's build command (" + build +
                            ") and answer with its output.";
            t.parameters = R"({"type":"object","properties":{}})";

            t.run = [argv, cap](const std::string&) -> std::string {
                std::string got, err;

                try {
                    const int status = sys::run(argv, got, err);

                    // Reported rather than swallowed: a build that failed is
                    // the answer the model asked for.
                    return capped("exit " + std::to_string(status) + "\n" +
                                  got + err, cap);
                }
                catch(std::exception& e) {
                    return std::string("error: could not run the build: ") +
                           e.what();
                }
            };

            out.push_back(t);
        }
    }

    return out;
}

conversation converse(std::vector<ai::message> turns,
                      const std::vector<tool>& tools,
                      const exchange& send,
                      unsigned int max_rounds)
{
    conversation out;

    const std::string declared = declare(tools);

    for(;;) {
        if(out.rounds >= max_rounds) {
            out.turns = turns;
            out.why = ending::bounded;
            out.detail = "stopped after " + std::to_string(max_rounds) +
                         " round(s) with the model still asking for tools";

            return out;
        }

        out.rounds++;

        ai::openai::answer a;

        try { a = send(turns, declared); }
        catch(std::exception& e) {
            out.turns = turns;
            out.why = ending::failed;
            out.detail = e.what();

            return out;
        }

        // The model's own turn, carrying the calls it made -- see converse()
        // in the header for why the calls and not only their results.
        ai::message said;

        said.role = "assistant";
        said.content = a.content;

        for(std::size_t i = 0; i < a.calls.size(); i++)
            said.tool_calls.push_back({ a.calls[i].id, a.calls[i].name,
                                        a.calls[i].arguments });

        turns.push_back(said);

        if(a.calls.empty()) {
            out.turns = turns;
            out.text = a.content;
            out.why = ending::answered;

            return out;
        }

        for(std::size_t i = 0; i < a.calls.size(); i++) {
            const ai::openai::call& c = a.calls[i];

            ran did;

            did.name = c.name;
            did.arguments = c.arguments;

            std::string result;

            const tool* found = 0;

            for(std::size_t j = 0; j < tools.size(); j++)
                if(tools[j].name == c.name) { found = &tools[j]; break; }

            if(!found)
                // Sent back rather than dropped: a model waiting for an answer
                // it will never get asks again, forever, and the bound above
                // becomes the only thing that stops it.
                result = "error: no tool called \"" + c.name + "\"";
            else {
                did.known = true;

                try { result = found->run(c.arguments); }
                catch(std::exception& e) {
                    did.failed = e.what();

                    result = std::string("error: ") + e.what();
                }
            }

            out.calls.push_back(did);

            ai::message back;

            back.role = "tool";
            back.content = result;

            turns.push_back(back);
        }
    }
}

reply parse(const std::string& text, const std::vector<std::string>& known) {
    reply out;

    // **Lifted out before the edit parser sees the text**, because to that
    // parser a `<tool_call>` block is prose: it would be skipped in silence
    // and the reply would look like one containing nothing.
    //
    // What is left is parsed as usual, so a model that wrote a file *and*
    // asked for a call gets both read.
    std::string rest = text;

    out.calls = ai::openai::calls_in(text, rest);

    std::istringstream in(rest);

    std::string line;

    // The last non-blank line seen outside a block: the filename, when the
    // model followed the format.
    std::string candidate;

    // The last name that worked, for a block that arrives without one -- a
    // model correcting itself usually repeats the file it was already on.
    std::string previous;

    bool inside = false;

    std::string name;
    std::string content;
    std::vector<std::string> why;

    while(std::getline(in, line)) {
        // getline strips the newline; keep the file's own line endings out of
        // the comparison by trimming only for the fence test.
        std::string info;

        if(fenced(line, info)) {
            if(!inside) {
                why.clear();

                name = clean(candidate, known, why);

                if(name.empty()) {
                    if(!previous.empty()) {
                        name = previous;

                        why.push_back("no filename before the fence; used the "
                                      "one before it: " + previous);
                    }
                    else if(known.size() == 1) {
                        name = known.front();

                        why.push_back("no filename before the fence; only one "
                                      "file was sent: " + known.front());
                    }
                }

                inside = true;
                content.clear();

                continue;
            }

            // Closing.
            if(name.empty()) {
                out.refusals.push_back({ std::string(),
                                         "a fenced block with no filename "
                                         "before it, and nothing to infer one "
                                         "from" });
            }
            else {
                edit e;

                e.name = name;
                e.content = content;
                e.guesses = why;

                out.edits.push_back(e);

                previous = name;
            }

            inside = false;
            candidate.clear();

            continue;
        }

        if(inside) {
            content += line;
            content += "\n";

            continue;
        }

        if(!util::trim(line).empty()) candidate = line;
    }

    // **The one that matters.**  A reply that stopped inside a block was cut
    // short -- the context wall, an interrupted generation, a dropped
    // connection -- and what is in hand is the beginning of a file.  Writing
    // it would leave a corrupted file where a whole one was.
    if(inside) {
        out.refusals.push_back({ name,
                                 "the reply ended inside a fenced block, so "
                                 "this file is only as complete as the reply "
                                 "was; nothing was written" });
    }

    return out;
}

namespace {

/** The resolved directory a name lands in, or empty if it cannot be had. */
std::string parent_of(const std::string& root, const std::string& name) {
    const std::string full = root + "/" + name;

    const std::string::size_type at = full.find_last_of('/');

    const std::string dir = at == std::string::npos ? full : full.substr(0, at);

    char buf[PATH_MAX];

    if(!::realpath(dir.c_str(), buf)) return std::string();

    return std::string(buf);
}

bool inside_root(const std::string& resolved_root, const std::string& dir) {
    if(dir == resolved_root) return true;

    // The separator matters: /rootlike must not pass for being under /root.
    return dir.size() > resolved_root.size() &&
           dir.compare(0, resolved_root.size(), resolved_root) == 0 &&
           dir[resolved_root.size()] == '/';
}

std::string slurp(const std::string& path, bool& found) {
    std::ifstream in(path, std::ios::binary);

    found = bool(in);

    if(!found) return std::string();

    std::ostringstream all;

    all << in.rdbuf();

    return all.str();
}

}

std::vector<result> apply(const reply& r, const std::string& root, bool dry_run)
{
    std::vector<result> out;

    char rootbuf[PATH_MAX];

    const bool have_root = ::realpath(root.c_str(), rootbuf) != 0;

    const std::string resolved_root = have_root ? std::string(rootbuf)
                                                : std::string();

    for(const refusal& f : r.refusals)
        out.push_back({ f.name, outcome::refused, f.why, {} });

    for(const edit& e : r.edits) {
        result one;

        one.name = e.name;
        one.guesses = e.guesses;
        one.what = outcome::refused;

        if(!have_root) {
            one.why = "the root \"" + root + "\" does not resolve";

            out.push_back(one);

            continue;
        }

        if(e.name.empty() || e.name[0] == '/') {
            one.why = "an absolute path, or none at all: \"" + e.name + "\"";

            out.push_back(one);

            continue;
        }

        // Resolution rather than inspection: `..`, a symlink pointing out and
        // an absolute path are one question, and the filesystem answers it.
        // A check that reads the text instead loses to the filesystem, which
        // is what #262 was.
        const std::string dir = parent_of(resolved_root, e.name);

        if(dir.empty()) {
            one.why = "the directory for \"" + e.name + "\" does not exist";

            out.push_back(one);

            continue;
        }

        if(!inside_root(resolved_root, dir)) {
            one.why = "\"" + e.name + "\" resolves to " + dir +
                      ", which is outside " + resolved_root;

            out.push_back(one);

            continue;
        }

        const std::string full = resolved_root + "/" + e.name;

        bool found = false;

        const std::string had = slurp(full, found);

        if(found && had == e.content) {
            // Not "written".  The model returned what it was given, and
            // saying it applied an edit would be the harness lie.
            one.what = outcome::unchanged;

            out.push_back(one);

            continue;
        }

        one.what = found ? outcome::written : outcome::created;

        if(!dry_run) {
            std::ofstream to(full, std::ios::binary | std::ios::trunc);

            if(!to) {
                one.what = outcome::refused;
                one.why = "could not open \"" + full + "\" to write";

                out.push_back(one);

                continue;
            }

            to << e.content;

            to.flush();

            if(!to) {
                one.what = outcome::refused;
                one.why = "the write to \"" + full + "\" failed";
            }
        }

        out.push_back(one);
    }

    return out;
}

}
}
}
