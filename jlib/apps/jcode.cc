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
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>

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

/**
 * The filename both prompts show when there is no real one to show (#465).
 *
 * **A model copies it.** Measured: shown this as the example and given one
 * file in a subdirectory, Qwen2.5-Coder-7B wrote `path/to/filename.cc`
 * verbatim in nine hunk replies of ten -- for `ringbuffer.hh` as well -- and
 * every one was refused for its name. So the prompt shows the real file
 * instead when exactly one was sent, and clean() knows this one for what it
 * is when the prompt still had to show it.
 */
const std::string PLACEHOLDER = "path/to/filename.cc";

/** Its directory, which a model sends back in front of a real name. */
const std::string PLACEHOLDER_DIR = "path/to/";

/**
 * The prompt's example, or the name in it: `path/to/filename.cc`, and the
 * same with no directory or another extension, because a model adapts the
 * one and drops the other. Only ever asked of a name no file sent has.
 */
bool placeholder(const std::string& name) {
    std::string rest = name;

    if(rest.compare(0, PLACEHOLDER_DIR.size(), PLACEHOLDER_DIR) == 0)
        rest = rest.substr(PLACEHOLDER_DIR.size());

    if(rest.compare(0, 8, "filename") != 0) return false;

    return rest.size() == 8 ||
           (rest[8] == '.' && rest.find('/') == std::string::npos);
}

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

/**
 * Whether a fence's own language contradicts the file it would be written to.
 *
 * **The model said both things**: ```sh on the fence and, by omission, a `.c`
 * file. One of them is wrong and it is not jcode's job to decide which -- but
 * it is jcode's job not to write a shell snippet over a source file, which is
 * what happened (#364): forty-one lines of C became one line of `make`,
 * because an illustration was the last fenced block in the reply.
 *
 * **Narrow on purpose.** Only a *recognised* language against a *recognised*
 * extension can disagree. An info string nobody here knows, or none at all,
 * says nothing and stays lenient -- models fence source with no info string
 * constantly and that has to keep working.
 *
 * The table is what has actually been seen rather than a survey of Markdown:
 * shells and build output because that is what a model illustrates a command
 * with, and the source languages because those are what it would overwrite.
 */
bool fence_contradicts(const std::string& info, const std::string& name) {
    if(info.empty()) return false;

    // A fence may carry more than a language -- ```c title=x -- and the
    // language is the first word.
    std::string lang = util::trim(info);

    const std::string::size_type sp = lang.find_first_of(" \t");

    if(sp != std::string::npos) lang = lang.substr(0, sp);

    for(std::size_t i = 0; i < lang.size(); i++)
        lang[i] = char(std::tolower((unsigned char)lang[i]));

    const std::string::size_type dot = name.find_last_of('.');

    if(dot == std::string::npos) return false;     // no extension to disagree

    std::string ext = name.substr(dot + 1);

    for(std::size_t i = 0; i < ext.size(); i++)
        ext[i] = char(std::tolower((unsigned char)ext[i]));

    struct { const char* lang; const char* exts; } known_langs[] = {
        { "sh",         "sh bash zsh" },
        { "bash",       "sh bash" },
        { "zsh",        "sh zsh" },
        { "shell",      "sh bash zsh" },
        { "console",    "sh bash zsh" },
        { "session",    "sh bash zsh" },
        { "make",       "mk" },
        { "makefile",   "mk" },
        { "diff",       "diff patch" },
        { "patch",      "diff patch" },
        { "json",       "json" },
        { "yaml",       "yaml yml" },
        { "yml",        "yaml yml" },
        { "xml",        "xml" },
        { "python",     "py" },
        { "py",         "py" },
        { "c",          "c h" },
        { "cpp",        "cc cpp cxx hh hpp h" },
        { "c++",        "cc cpp cxx hh hpp h" },
        { "cc",         "cc cpp cxx hh hpp h" },
        { "objc",       "m h" },
        { "objcpp",     "mm hh h" },
    };

    for(const auto& k : known_langs) {
        if(lang != k.lang) continue;

        // Recognised language: does the extension appear in its set?
        const std::string set = std::string(" ") + k.exts + " ";

        return set.find(" " + ext + " ") == std::string::npos;
    }

    return false;     // a language nobody here knows says nothing
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

    if(name.empty() || std::find(known.begin(), known.end(), name) != known.end())
        return name;

    // **The prompt's own example, sent back whole** (#465). There is no such
    // file, and with only one file sent there is no question which was
    // meant; with several there is, and the name is left to match nothing.
    if(placeholder(name)) {
        if(known.size() == 1) {
            why.push_back("\"" + name + "\" is the name in the prompt's "
                          "example, not a file; only one file was sent: " +
                          known[0]);

            return known[0];
        }

        return name;
    }

    // **Or its directory, in front of a real name.** Shown `path/to/`, a
    // model sends it back: `path/to/hello.cc` for a file at the root, and
    // measured, `path/to/quant.hh` for `jlib/ai/quant.hh`.
    std::string rest = name;

    const bool example = rest.compare(0, PLACEHOLDER_DIR.size(),
                                      PLACEHOLDER_DIR) == 0;

    if(example) rest = rest.substr(PLACEHOLDER_DIR.size());

    // **And the end of a path that was sent**, whole components at a time:
    // `quant.hh` or `ai/quant.hh` for `jlib/ai/quant.hh`, never `i/quant.hh`,
    // and only when one file ends that way. This took the basename alone
    // and compared it against the paths sent, which is why `path/to/quant.hh`
    // was refused: every path sent had a directory in front.
    //
    // **What is deliberately not done** is the other half of what that rule
    // did: take a basename behind *any* directory. `src/hello.cc` for
    // `hello.cc` became `hello.cc` -- and `src/hello.cc` may be the file the
    // model means to create. Only `path/to/` is known to be no directory at
    // all.
    std::vector<std::string> ends;

    for(const std::string& k : known)
        if(k == rest ||
           (k.size() > rest.size() && k[k.size() - rest.size() - 1] == '/' &&
            k.compare(k.size() - rest.size(), rest.size(), rest) == 0))
            ends.push_back(k);

    if(ends.size() != 1) return name;

    if(example)
        why.push_back("\"" + PLACEHOLDER_DIR + "\" is the directory in the "
                      "prompt's example, not a real one; took \"" + ends[0] +
                      "\", the file sent whose path ends \"" + rest + "\"");
    else
        why.push_back("no file called \"" + name + "\" was sent; took \"" +
                      ends[0] + "\", the one file sent whose path ends that way");

    return ends[0];
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

std::string system_prompt(const std::vector<tool>& tools, format want,
                          const std::string& example)
{
    // The real file when there is one to show, because the example is what
    // a model copies (#465); see PLACEHOLDER.
    const std::string shown = example.empty() ? PLACEHOLDER : example;

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
    if(want == format::hunks)
        return
            "You are a careful programmer working on an existing codebase.\n"
            "\n"
            "When you change a file you MUST answer with search and replace "
            "blocks, in this format and no other:\n"
            "\n" +
            shown + "\n"
            "<<<<<<< SEARCH\n"
            "the lines exactly as they are now\n"
            "=======\n"
            "what they should be instead\n"
            ">>>>>>> REPLACE\n"
            "\n"
            "- the filename goes on a line of its own, exactly as it was "
            "given to you\n"
            "- **the SEARCH text must be character for character what is in "
            "the file now**, including indentation: it is looked for, not "
            "interpreted, and a block that does not match is not applied\n"
            "- give enough lines for the SEARCH text to appear exactly once "
            "in the file; a block that matches twice is not applied either\n"
            "- use one block per change, and as many blocks as you need\n"
            "- do not return the whole file, and do not abbreviate with "
            "\"...\": only the lines you are changing\n"
            "- if a change is not needed, or the request is unclear, say so "
            "in prose and return no blocks at all\n" + asking;

    return
        "You are a careful programmer working on an existing codebase.\n"
        "\n"
        "When you change a file you MUST return its entire new contents, in "
        "this format and no other:\n"
        "\n" +
        shown + "\n"
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
             std::size_t budget, const std::vector<tool>& tools, format want)
{
    plan out;

    // **The example names the file, when there is one** (#465). With
    // several, it does not: a model copies the example's name, and given the
    // first file's name, a block meant for the second would be written to the
    // first -- a whole-file one over it. `path/to/filename.cc` is a name no
    // file has, so copying it costs a refusal rather than a file.
    const std::string system =
        system_prompt(tools, want, files.size() == 1 ? files[0].name
                                                     : std::string());

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

const char* spell(settled s) {
    switch(s) {
    case settled::built:   return "the build passes";
    case settled::bounded: return "out of attempts";
    case settled::stalled: return "the model stopped changing anything";
    case settled::nothing: return "nothing was written";
    case settled::failed:  return "the conversation failed";
    }

    return "?";
}

attempts until_built(std::vector<ai::message> turns,
                     const std::vector<tool>& tools,
                     const exchange& send,
                     const writer& write,
                     const builder& build,
                     const std::vector<std::string>& known,
                     unsigned int max_attempts,
                     unsigned int max_rounds)
{
    attempts out;

    // What the last attempt put on disk, so an attempt that repeats itself
    // can be recognised.  Compared by content rather than by name: a model
    // that sends back the same bytes has nothing more to offer, whatever it
    // calls them.
    std::vector<std::pair<std::string, std::string> > before;

    while(out.made < max_attempts) {
        out.made++;

        const conversation talk =
            converse(turns, tools, send, max_rounds);

        out.calls.insert(out.calls.end(), talk.calls.begin(), talk.calls.end());

        if(talk.why != ending::answered) {
            out.why = settled::failed;
            out.detail = std::string(spell(talk.why)) +
                         (talk.detail.empty() ? "" : ": " + talk.detail);

            return out;
        }

        const reply parsed = parse(talk.text, known);

        std::vector<std::pair<std::string, std::string> > now;

        for(std::size_t i = 0; i < parsed.edits.size(); i++)
            now.push_back(std::make_pair(parsed.edits[i].name,
                                         parsed.edits[i].content));

        // Sorted, because the same edits in a different order are the same
        // edits and a model has no reason to keep one.
        std::sort(now.begin(), now.end());

        if(!parsed.edits.empty() && !before.empty() && now == before) {
            out.why = settled::stalled;
            out.detail = "attempt " + std::to_string(out.made) +
                         " sent the same files as the one before it";

            return out;
        }

        before = now;

        // **The writer sees every reply, including one with nothing in it.**
        // Saying what was refused is part of what it does, and a reply that
        // stopped mid-file is all refusal and no edit -- returning early here
        // meant the one thing worth telling the user was never printed.
        if(!write(parsed)) {
            out.why = settled::nothing;
            out.detail = parsed.edits.empty()
                             ? "the model returned no files"
                             : "nothing was taken from that reply";

            return out;
        }

        const built check = build();

        out.last = check.output;

        if(check.passed) { out.why = settled::built; return out; }

        // **The compiler's answer, back to the model.**  A user turn rather
        // than a tool result: the model did not ask for this, and a `tool`
        // turn with no call before it is the orphan #336 is about.
        turns = talk.turns;

        ai::message said;

        said.role = "user";
        said.content = "That did not work. The build says:\n\n" +
                       check.output +
                       "\n\nReturn the whole file again with this fixed.";

        turns.push_back(said);
    }

    out.why = settled::bounded;
    out.detail = "stopped after " + std::to_string(max_attempts) +
                 " attempt(s) with the build still failing";

    return out;
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

            // **-F, because this searches for a string and says so.**
            //
            // Without it grep reads the pattern as a basic regular
            // expression, and a model looking for code writes code: asked for
            // `void reverse(char* s)` it matched nothing at all, because
            // `char*` there means "cha and then zero or more r". Every
            // interesting search a coding model makes contains `*`, `(`, `.`
            // or `[`, and every one of them silently found nothing.
            //
            // `--` as well, so a pattern starting with a dash is a pattern
            // rather than a flag, and through sys::run rather than a shell.
            std::vector<std::string> argv{ "grep", "-rnIF", "--", want,
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

namespace {

/** The first line of a hunk's search text, for a refusal worth reading. */
std::string first_line_of(const std::string& s) {
    const std::string::size_type at = s.find('\n');

    const std::string one = at == std::string::npos ? s : s.substr(0, at);

    return one.size() > 60 ? one.substr(0, 60) + "..." : one;
}

}

namespace {

/**
 * Search/replace blocks, lifted out before the whole-file parser runs.
 *
 * The same arrangement tool calls get, and for the same reason: to the
 * whole-file parser these markers are prose, so it would skip them in silence
 * and report a reply with nothing in it. Lifting them first lets both formats
 * stay lenient without either having to know about the other.
 *
 * Recognised leniently -- a line beginning `<<<<<<<`, one that trims to
 * `=======`, one beginning `>>>>>>>`. Models put varying words after the
 * angle brackets and aider's own prompt shows two spellings.
 *
 * @param[out] left the text with the blocks removed
 */
std::vector<edit> hunks_in(const std::string& text, std::string& left,
                           const std::vector<std::string>& known,
                           std::vector<std::string>& why)
{
    std::vector<edit> out;

    left.clear();

    std::istringstream in(text);

    std::string line, previous, name;

    // Lines held back because they might turn out to be a filename: the one
    // before an opening marker belongs to the block, not to the prose.
    std::string pending;

    // Whether one is held at all. **An empty line is a line too**: this used
    // to test `pending` for being empty, so an empty line was held and never
    // let go, and every one in the reply was gone before the whole-file
    // parser saw it -- every file jcode wrote whole, from #365 until #464
    // found it, lost every empty line it had, and kept only the blank lines
    // that held a space. Nothing that judged an edit by its non-blank lines
    // could see it, and nothing did.
    bool held = false;

    while(std::getline(in, line)) {
        const std::string trimmed = util::trim(line);

        if(trimmed.compare(0, 7, "<<<<<<<") != 0) {
            if(held) left += pending + "\n";

            pending = line;
            held = true;

            if(!trimmed.empty()) previous = trimmed;

            continue;
        }

        // The line before the marker is the filename, when there was one.
        std::string who = util::trim(pending);

        pending.clear();
        held = false;

        // What was assumed to arrive at *this block's* name, which is
        // attached to the edit it names -- see below.
        std::vector<std::string> said;

        // **Cleaned by the same function the whole-file parser uses** (#457).
        // This took the line raw, so every envelope clean() absorbs -- bold,
        // backticks, a heading, a trailing colon, and the `path/to/` prefix
        // pasted back out of the prompt's own example -- was refused in
        // hunks and accepted in whole. Measured against a real model: the
        // prefix one is not exotic, it is what Qwen2.5-Coder-7B sent for a
        // one-line edit, and the edit was refused at containment.
        //
        // clean() also carries the careful version of "a space means prose",
        // which this had open-coded as `find(' ')` -- the careful one lets an
        // exact match against a file that was sent win over the heuristic,
        // so a filename with a space in it survives.
        if(!who.empty()) who = clean(who, known, said);

        if(who.empty()) {
            if(!name.empty())
                said.push_back("no filename before the search block; used the "
                               "one before it: " + name);
            else if(known.size() == 1) {
                name = known[0];

                said.push_back("no filename before the search block; only one "
                               "file was sent: " + name);
            }

            who = name;
        }

        if(who.empty()) {
            // Nothing to attach it to. The text is kept rather than dropped.
            left += line + "\n";

            continue;
        }

        name = who;

        hunk h;

        bool in_replace = false, closed = false;

        while(std::getline(in, line)) {
            const std::string t = util::trim(line);

            if(!in_replace && t == "=======") { in_replace = true; continue; }

            if(t.compare(0, 7, ">>>>>>>") == 0) { closed = true; break; }

            (in_replace ? h.replace : h.search) += line + "\n";
        }

        // **An unterminated block is not an edit.** The same hazard the
        // whole-file parser refuses a half-file for: what is in hand is the
        // beginning of a change, and applying it would be applying half.
        if(!closed) {
            why.push_back(name + ": a search block that never closed was "
                                 "dropped");

            continue;
        }

        // Attach to the edit for this file, so several hunks on one file are
        // one edit applied in order.
        edit* at = 0;

        for(std::size_t i = 0; i < out.size(); i++)
            if(out[i].name == name) { at = &out[i]; break; }

        if(!at) { out.push_back(edit()); at = &out.back(); at->name = name; }

        at->hunks.push_back(h);

        // **Against the edit they are about.** Every guess went into one list
        // that parse() pinned to the *first* edit, so with two files a guess
        // about the second was recorded against the first -- and
        // `edit::guesses` is documented as what was assumed to arrive at
        // *that* edit's name. Once each: three hunks on one file whose name
        // was bolded three times are one guess, not three.
        for(const std::string& g : said)
            if(std::find(at->guesses.begin(), at->guesses.end(), g) ==
               at->guesses.end())
                at->guesses.push_back(g);
    }

    if(held) left += pending + "\n";

    return out;
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

    // Then the search/replace blocks, for the same reason and in the same
    // way: what the whole-file parser cannot read it would skip in silence.
    std::vector<std::string> hunk_guesses;

    std::string after_hunks;

    out.edits = hunks_in(rest, after_hunks, known, hunk_guesses);

    if(!out.edits.empty()) {
        // A reply is in one format or the other. Having found hunks, the rest
        // is prose -- and running the whole-file parser over it would read a
        // fenced example as a file to write.
        // What is left in the shared list is about blocks that produced no
        // edit at all -- one that never closed -- so there is no edit of its
        // own to carry it, and the first one does. Name guesses are not here
        // any more: hunks_in attaches each to the edit it is about.
        for(std::size_t i = 0; i < hunk_guesses.size(); i++)
            out.edits[0].guesses.push_back(hunk_guesses[i]);

        return out;
    }

    std::istringstream in(after_hunks);

    std::string line;

    // The last non-blank line seen outside a block: the filename, when the
    // model followed the format.
    std::string candidate;

    // The last name that worked, for a block that arrives without one -- a
    // model correcting itself usually repeats the file it was already on.
    std::string previous;

    bool inside = false;

    // Whether this block's filename was inferred, and what language its
    // opening fence declared.  Together they are how an illustration is told
    // from an edit; see fence_contradicts.
    bool guessed = false;

    std::string opened_with;

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

                // Stated or inferred, which decides how much this trusts the
                // block below -- see fence_contradicts.
                guessed = name.empty();

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

                // The **opening** fence's language.  fenced() fills `info` on
                // the closing line too, where it is almost always empty, so
                // the one that matters has to be kept.
                opened_with = info;

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
            // **A guessed filename and a contradicting language is not an
            // edit.**  The model illustrated something -- a command, a patch,
            // a config -- and jcode was about to write it over a source file
            // because that file was the only one sent. #364.
            else if(guessed && fence_contradicts(opened_with, name)) {
                out.refusals.push_back({ name,
                                         "a ```" + opened_with + " block with "
                                         "no filename before it, which cannot "
                                         "be the contents of \"" + name +
                                         "\" -- read as an illustration "
                                         "rather than an edit" });
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

    // **One block per file wins, and it is the last.**
    //
    // A reply that illustrates before it answers produces several blocks for
    // the same name -- measured, a model asked to fix three functions wrote
    // six snippets and then the file, and jcode announced seven writes of
    // util.c. apply() wrote them in order, so the file ended as the last one
    // anyway; what the other six bought was six announcements of a write that
    // did not survive, and six passes over the disk.
    //
    // The last is kept because that is what the sequence already produced,
    // and because a model correcting itself mid-reply means the correction.
    // The superseded ones are refusals rather than silence: a reply needing
    // this is not in the format that was asked for, and the user should see
    // how far off it was.
    std::vector<edit> kept;

    for(std::size_t i = 0; i < out.edits.size(); i++) {
        std::size_t later = out.edits.size();

        for(std::size_t j = i + 1; j < out.edits.size(); j++)
            if(out.edits[j].name == out.edits[i].name) { later = j; break; }

        if(later == out.edits.size()) { kept.push_back(out.edits[i]); continue; }

        out.refusals.push_back({ out.edits[i].name,
                                 "a later block in the same reply gave this "
                                 "file again, so this one was superseded" });
    }

    out.edits.swap(kept);

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

namespace {

/** The lines of a text, each keeping its newline; a last line may have none. */
std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;

    std::size_t from = 0;

    while(from < text.size()) {
        const std::size_t nl = text.find('\n', from);

        if(nl == std::string::npos) { out.push_back(text.substr(from)); break; }

        out.push_back(text.substr(from, nl + 1 - from));

        from = nl + 1;
    }

    return out;
}

/** A line as placement compares it: no trailing space, tab, CR or newline. */
std::string bare(const std::string& line) {
    std::size_t n = line.size();

    while(n && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                line[n - 1] == ' ' || line[n - 1] == '\t')) n--;

    return line.substr(0, n);
}

bool blank(const std::string& line) {
    for(char c : line) if(!std::isspace(static_cast<unsigned char>(c))) return false;

    return true;
}

typedef std::vector<std::pair<std::size_t, std::size_t> > pairs_t;

/**
 * The most cells align() will spend lining two runs up: sixteen megabytes.
 *
 * Its table has a row for each line of the block's run and a column for each
 * of the file's, and a run is what lies between two lines unique to both --
 * a few lines, in code, even when the file side is the rest of a long file --
 * less whatever the two share at their start, which costs nothing. What
 * comes near this is thousands of lines alike on both sides and changed near
 * their start: a table of data, not code, and refused rather than paid for.
 */
const std::size_t ALIGN_CELLS = std::size_t(1) << 22;

/**
 * Line a run of the block, `b`, up against a run of the file, `a`, from the
 * start of each. Pairs are (line in a, line in b).
 *
 * **Between two anchors** (`to_edge` false) all of `a` is the block's to
 * account for, and the most lines in common wins, matched as early in `a` as
 * they can be -- so that two snippets pasted together show the run of the
 * file between them as the gap it is, not as a match against the copy of
 * `return y; }` in the function they left out.
 *
 * **Beyond the first or last anchor** (`to_edge`) `a` runs on to the edge of
 * the file, and only part of it is the block's: the part that ends on the
 * run's own last line -- the block's last line, or its first, which place()
 * hands over with both runs reversed -- and of those the **cheapest to
 * account for**. Each line of the file the block passes over costs one, as
 * does each line of the block that matches nothing, and the file beyond the
 * part is free; between two that cost the same, the one with more lines in
 * common. When that line is nowhere in `a`, the cheapest part of any kind,
 * and place() decides what an unmatched end means.
 *
 * Neither half is decoration. Most lines in common was the first rule here,
 * and it matched a block that added a line to a function and changed its
 * `return y;` to the `return x;` two later functions had to theirs, and wrote
 * it over the three functions in between: cheapest is what stops that.
 * Cheapest alone was the second, and it refused a function whose last two
 * lines became one, because leaving both of the block's lines unmatched cost
 * less than replacing three lines with two, and an unmatched last line is a
 * part with no known end: ending on the run's own last line is what stops
 * that.
 *
 * False, with nothing matched, when the two are too long to line up within
 * ALIGN_CELLS.
 */
bool align(const std::vector<std::size_t>& a, const std::vector<std::size_t>& b,
           bool to_edge, pairs_t& out)
{
    out.clear();

    // A start the two share is matched as it stands, which is where either
    // rule would put it anyway: a run the block reproduces is never the
    // table's to pay for.
    std::size_t same = 0;

    while(same < a.size() && same < b.size() && a[same] == b[same]) {
        out.push_back({ same, same });
        same++;
    }

    const std::size_t n = b.size() - same, m = a.size() - same;

    if(!n || !m) return true;

    if(n + 1 > ALIGN_CELLS / (m + 1)) { out.clear(); return false; }

    std::vector<unsigned> table((n + 1) * (m + 1), 0);

    auto dp = [&](std::size_t x, std::size_t y) -> unsigned& {
        return table[x * (m + 1) + y];
    };

    // The rest, from where they first differ: element x of the block's run
    // is b[same + x - 1] here, and y of the file's a[same + y - 1].
    auto same_line = [&](std::size_t x, std::size_t y) {
        return b[same + x - 1] == a[same + y - 1];
    };

    for(std::size_t x = 1; x <= n; x++)
        for(std::size_t y = 1; y <= m; y++)
            dp(x, y) = same_line(x, y) ? dp(x - 1, y - 1) + 1
                                       : std::max(dp(x - 1, y), dp(x, y - 1));

    // How much of the file's run is the block's: y lines of it, and the
    // block's x0 lines lined up within them.
    std::size_t y = 0, x0 = n;

    pairs_t rest;

    if(to_edge) {
        // k lines of the file with `matched` of them in common cost
        // (k - matched) + (n - matched).
        auto cost = [&](std::size_t k, std::size_t matched) {
            return k + n - 2 * matched;
        };

        // Ending on the block's last line, matched to the file's line k.
        std::size_t end = 0;

        for(std::size_t k = 1; k <= m; k++) {
            if(!same_line(n, k)) continue;

            const std::size_t now = dp(n - 1, k - 1) + 1;
            const std::size_t was = end ? dp(n - 1, end - 1) + 1 : 0;

            if(!end || cost(k, now) < cost(end, was) ||
               (cost(k, now) == cost(end, was) && now > was))
                end = k;
        }

        if(end) {
            rest.push_back({ same + end - 1, same + n - 1 });

            y = end - 1;
            x0 = n - 1;
        }
        else
            for(std::size_t k = 1; k <= m; k++)
                if(cost(k, dp(n, k)) < cost(y, dp(n, y)) ||
                   (cost(k, dp(n, k)) == cost(y, dp(n, y)) && dp(n, k) > dp(n, y)))
                    y = k;
    }
    else
        // The shortest stretch of the run that does as well as all of it.
        while(dp(n, y) < dp(n, m)) y++;

    for(std::size_t x = x0; x && y;) {
        if(same_line(x, y) && dp(x, y) == dp(x - 1, y - 1) + 1) {
            rest.push_back({ same + y - 1, same + x - 1 });
            x--;
            y--;
        }
        else if(dp(x - 1, y) >= dp(x, y - 1)) x--;
        else y--;
    }

    out.insert(out.end(), rest.rbegin(), rest.rend());

    return true;
}

/** `1 line`, `4 lines`. */
std::string lines(std::size_t n) {
    return std::to_string(n) + (n == 1 ? " line" : " lines");
}

/** Where a whole-file block goes in the file, or why it cannot go anywhere. */
struct placement {
    bool ok = false;
    std::string content;      ///< the file as it would be
    std::size_t from = 0;     ///< the first line replaced, from one
    std::size_t to = 0;       ///< and the last
    std::string why;          ///< when not ok
};

/**
 * Put a block where it belongs in the file, rather than over all of it (#464).
 *
 * ## The assumption this replaces
 *
 * A whole-file block was taken to *be* the whole file, and that one
 * assumption was the only way jcode could destroy one. Asked for whole files,
 * a model sometimes returns only the part it changed, and the measurement in
 * tools/edit-formats watched that turn a 1074-line openai.cc into the 22
 * lines of one function, a 203-line sampler.cc into 8, and a 297-line
 * listener.cc into the same file without its licence and `#include`s -- each
 * reported `written`, and in two of the three the change itself was right.
 *
 * So a block replaces **the part of the file it accounts for**. A well-formed
 * whole file accounts for all of it and replaces it, exactly as before; a
 * block that is one function replaces that function.
 *
 * ## How it is placed
 *
 * By the lines that occur exactly once in the file and exactly once in the
 * block, chained in order -- patience diff's idea, and for the same reason: a
 * unique line says where it is, and `}` or `return true;` says nothing. Every
 * other line of the block is then lined up against the file's, between two of
 * those anchors and beyond the first and last, by align() -- whose comment
 * says why those are two different rules, each learned from a case the other
 * got wrong. That is how a function's closing brace is found even when the
 * line before it changed, which the first version, widening each anchor over
 * identical neighbours, could not do.
 *
 * **Blank lines are invisible to all of it.** They carry no position, and
 * models add and drop them freely: an earlier version counted them, and
 * refused two correct whole-file edits because the model had collapsed a
 * blank line at the end of each.
 *
 * ## What a part of the file may do
 *
 * Change what it shows, and nothing else. At each edge, the block's own lines
 * beyond what it shares with the file either do not exist -- it is a part,
 * and the file's lines there are kept -- or are at least as many as the
 * file's there, and replace them: that is a whole file whose first or last
 * lines changed. Fewer is refused, because then where the block begins or
 * ends is not known; that is the shape of a fragment whose first line the
 * model rewrote, and of one with a new comment above the function, and
 * placing either by guesswork would corrupt the lines beside it.
 *
 * And inside, a part may not leave out a run of the file without putting
 * anything in its place. That is the seam between two snippets pasted
 * together, and deleting the code between them is the same destruction in
 * miniature. A *whole* file may leave a run out -- it reproduced everything
 * else, so the gap is a deletion -- which is how whole-file format deletes at
 * all.
 *
 * **What that costs, said out loud.** A fragment that deletes lines inside
 * the code it shows has exactly the seam's shape, and is refused: a whole
 * file or a hunk can delete, and a fragment cannot. And where a block ends
 * among lines that say nothing about where they are, it ends at the nearest,
 * and the file keeps the rest. So a whole file that deletes its last function
 * reads as one that left it out -- refused, or unchanged if that was all it
 * left out -- and one that gives one of its three closing braces a comment
 * keeps the brace as well, which is the one way placement writes a file
 * wrong. That one is a line kept rather than code lost, and not silent: the
 * question says `lines 1-19 of 20`, where a whole file says `20 to 20 lines`.
 *
 * ## The one comparison
 *
 * A block with no line in common with the file that occurs once in each
 * cannot be placed, and replaces the file only if it has at least as many
 * lines -- the edge rule above, applied to both edges at once. That is what
 * lets a one-line file be edited at all, and what refuses a block holding
 * only a new function instead of writing it over everything. It also refuses
 * a whole file rewritten shorter that keeps no line the old one had once
 * only, which is the price.
 */
placement place(const std::string& had, const std::string& want) {
    placement p;

    const std::vector<std::string> a = lines_of(had);
    const std::vector<std::string> b = lines_of(want);

    // The non-blank lines, each as a number -- one per distinct line, so that
    // lines compare as integers -- and where each one really is.
    std::unordered_map<std::string, std::size_t> id;

    auto number = [&](const std::string& line) {
        return id.emplace(bare(line), id.size()).first->second;
    };

    std::vector<std::size_t> ia, ib, na, nb;

    for(std::size_t k = 0; k < a.size(); k++)
        if(!blank(a[k])) { ia.push_back(k); na.push_back(number(a[k])); }

    for(std::size_t k = 0; k < b.size(); k++)
        if(!blank(b[k])) { ib.push_back(k); nb.push_back(number(b[k])); }

    std::vector<std::size_t> in_a(id.size(), 0), in_b(id.size(), 0);
    std::vector<std::size_t> where(id.size(), 0);

    for(std::size_t i = 0; i < na.size(); i++) { in_a[na[i]]++; where[na[i]] = i; }

    for(std::size_t x : nb) in_b[x]++;

    // (line in the file, line in the block) for every line unique to both,
    // in the block's order.
    pairs_t pairs;

    for(std::size_t j = 0; j < nb.size(); j++)
        if(in_a[nb[j]] == 1 && in_b[nb[j]] == 1) pairs.push_back({ where[nb[j]], j });

    // The longest chain of them in the file's order too: a line the block
    // moved is not evidence of where the block is.
    std::vector<std::size_t> tails;
    std::vector<std::size_t> prev(pairs.size(), pairs.size());

    for(std::size_t k = 0; k < pairs.size(); k++) {
        const auto at = std::lower_bound(tails.begin(), tails.end(), k,
                                         [&](std::size_t t, std::size_t q) {
                                             return pairs[t].first < pairs[q].first;
                                         });

        if(at != tails.begin()) prev[k] = *(at - 1);

        if(at == tails.end()) tails.push_back(k);
        else *at = k;
    }

    pairs_t anchor;

    for(std::size_t k = tails.empty() ? pairs.size() : tails.back();
        k < pairs.size(); k = prev[k])
        anchor.push_back(pairs[k]);

    std::reverse(anchor.begin(), anchor.end());

    if(anchor.empty()) {
        if(nb.size() >= na.size()) {
            p.ok = true;
            p.content = want;
            p.from = a.empty() ? 0 : 1;
            p.to = a.size();

            return p;
        }

        p.why = "this block is shorter than the file and has no line in "
                "common with it that occurs once in each, so where it goes is "
                "not known";

        return p;
    }

    // Every other line, lined up between the anchors and beyond them: the
    // file's lines a0 to a1 against the block's b0 to b1, from a0 -- or,
    // before the first anchor, from a1 backwards.
    pairs_t all;

    bool fits = true;

    auto line_up = [&](std::size_t a0, std::size_t a1, std::size_t b0,
                       std::size_t b1, bool backwards, bool to_edge) {
        std::vector<std::size_t> x(na.begin() + std::ptrdiff_t(a0),
                                   na.begin() + std::ptrdiff_t(a1));
        std::vector<std::size_t> y(nb.begin() + std::ptrdiff_t(b0),
                                   nb.begin() + std::ptrdiff_t(b1));

        if(backwards) {
            std::reverse(x.begin(), x.end());
            std::reverse(y.begin(), y.end());
        }

        pairs_t got;

        if(!align(x, y, to_edge, got)) fits = false;

        if(backwards) {
            std::reverse(got.begin(), got.end());

            for(auto& g : got) g = { a1 - 1 - g.first, b1 - 1 - g.second };
        }
        else
            for(auto& g : got) g = { a0 + g.first, b0 + g.second };

        all.insert(all.end(), got.begin(), got.end());
    };

    line_up(0, anchor.front().first, 0, anchor.front().second, true, true);

    for(std::size_t k = 0; k < anchor.size(); k++) {
        all.push_back(anchor[k]);

        const bool last = k + 1 == anchor.size();

        line_up(anchor[k].first + 1, last ? na.size() : anchor[k + 1].first,
                anchor[k].second + 1, last ? nb.size() : anchor[k + 1].second,
                false, last);
    }

    if(!fits) {
        p.why = "too many of this block's lines are alike to line it up with "
                "the file";

        return p;
    }

    const std::size_t i0 = all.front().first, j0 = all.front().second;
    const std::size_t i1 = all.back().first,  j1 = all.back().second;

    const std::size_t lead_b = j0, lead_a = i0;
    const std::size_t trail_b = nb.size() - 1 - j1, trail_a = na.size() - 1 - i1;

    if(lead_b > 0 && lead_b < lead_a) {
        p.why = (lead_b == 1 ? std::string("this block's first line is")
                             : "this block's first " + lines(lead_b) + " are") +
                " not in the file, so where it begins is not known";

        return p;
    }

    if(trail_b > 0 && trail_b < trail_a) {
        p.why = (trail_b == 1 ? std::string("this block's last line is")
                              : "this block's last " + lines(trail_b) + " are") +
                " not in the file, so where it ends is not known";

        return p;
    }

    const bool head = lead_b > 0 || lead_a == 0;     // reaches the file's start
    const bool tail = trail_b > 0 || trail_a == 0;   // and its end

    // Inside a part: two lines next to each other in the block, with a run of
    // the file between them, is a run the block left out.
    if(!(head && tail))
        for(std::size_t k = 0; k + 1 < all.size(); k++)
            if(all[k + 1].second == all[k].second + 1 &&
               all[k + 1].first > all[k].first + 1) {
                p.why = "this block leaves out " +
                        lines(all[k + 1].first - all[k].first - 1) +
                        " of the file between two parts it shows, and is not "
                        "the whole file";

                return p;
            }

    // In real lines now. At an edge the block reaches, it owns everything to
    // the file's edge, blank lines included; at an edge where it is only a
    // part, the file keeps its own lines -- and its own blank lines -- and
    // the block's are dropped there.
    const std::size_t s = head ? 0 : ia[i0];
    const std::size_t e = tail ? a.size() - 1 : ia[i1];
    const std::size_t bf = head ? 0 : ib[j0];
    const std::size_t bt = tail ? b.size() - 1 : ib[j1];

    std::string out;

    for(std::size_t k = 0; k < s; k++) out += a[k];

    for(std::size_t k = bf; k <= bt; k++) {
        out += b[k];

        // A block whose last line has no newline still needs one when the
        // file goes on after it.
        if(k == bt && !b[k].empty() && b[k].back() != '\n' && e + 1 < a.size())
            out += '\n';
    }

    for(std::size_t k = e + 1; k < a.size(); k++) out += a[k];

    p.ok = true;
    p.content = out;
    p.from = s + 1;
    p.to = e + 1;

    return p;
}

}

std::string extent(const result& r) {
    if(!r.before) return std::to_string(r.after) + " lines";

    if(r.from == 1 && r.to == r.before)
        return std::to_string(r.before) + " to " + std::to_string(r.after) +
               " lines";

    return "lines " + std::to_string(r.from) + "-" + std::to_string(r.to) +
           " of " + std::to_string(r.before);
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

        // **Hunks are resolved here, against the file, not at parse time.**
        // parse() promises a reply is data and touching disk is this
        // function's job -- so a search/replace edit arrives as the changes
        // to make and becomes content exactly here.
        std::string want = e.content;

        if(!e.hunks.empty()) {
            if(!found) {
                one.why = "\"" + e.name + "\" does not exist, so there is "
                          "nothing for a search block to match";

                out.push_back(one);

                continue;
            }

            want = had;

            bool bad = false;

            for(std::size_t h = 0; h < e.hunks.size() && !bad; h++) {
                const std::string& look = e.hunks[h].search;

                // An empty search would match at every position; a model that
                // wrote one meant something it did not say.
                if(look.empty()) {
                    one.why = "a search block with nothing to look for";
                    bad = true;

                    break;
                }

                const std::string::size_type at = want.find(look);

                if(at == std::string::npos) {
                    one.why = "a search block does not match anything in \"" +
                              e.name + "\": " + first_line_of(look);
                    bad = true;

                    break;
                }

                // **Twice is a refusal, not a coin toss.** Replacing the
                // first of two identical blocks is a choice the model did not
                // know it was making, and getting it wrong edits the wrong
                // part of the file.
                if(want.find(look, at + 1) != std::string::npos) {
                    one.why = "a search block matches more than once in \"" +
                              e.name + "\": " + first_line_of(look);
                    bad = true;

                    break;
                }

                want = want.substr(0, at) + e.hunks[h].replace +
                       want.substr(at + look.size());
            }

            if(bad) { out.push_back(one); continue; }
        }

        // **A whole-file block goes where it belongs**, not over the file
        // (#464) -- resolved here, against the file, for the same reason the
        // hunks above are: it needs the file, and parse() promises not to
        // touch one. See place() for the rule and what it costs.
        if(found && e.hunks.empty()) {
            const placement at = place(had, e.content);

            if(!at.ok) {
                // A block with no filename of its own was given this one, and
                // the person deciding what to do next should know that.
                bool guessed = false;

                for(const std::string& g : e.guesses)
                    if(g.find("no filename before the fence") != std::string::npos)
                        guessed = true;

                one.why = at.why +
                          (guessed ? std::string(" -- and it had no filename of "
                                                 "its own, so this one was "
                                                 "guessed")
                                   : std::string()) +
                          "; ask for the whole file, or for search/replace hunks";

                out.push_back(one);

                continue;
            }

            want = at.content;

            one.from = at.from;
            one.to = at.to;
        }

        if(found && had == want) {
            // Not "written".  The model returned what it was given, and
            // saying it applied an edit would be the harness lie.
            one.what = outcome::unchanged;

            out.push_back(one);

            continue;
        }

        one.before = found ? lines_of(had).size() : 0;
        one.after = lines_of(want).size();

        // A hunk edit, or a new file, is described by its size; only a placed
        // block names the lines it replaced.
        if(!one.from) {
            one.from = one.before ? 1 : 0;
            one.to = one.before;
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

            to << want;

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

bool deliver(const reply& r, const std::string& root, const teller& say,
             const asker& ask, bool dry_run)
{
    for(const std::string& g : r.guesses()) say("guessed: " + g);

    // Decided first, written second: a person is asked about an edit that is
    // already known to be applicable, rather than being asked and then told
    // it was refused.
    //
    // **Qualified, both calls, and not for tidiness.** An argument of type
    // std::string puts namespace std in the lookup, and `std::apply` is a
    // perfect-forwarding template -- so for a non-const `reply` it beats
    // jcode's `const reply&` overload and fails to compile, from inside
    // <tuple>, nowhere near the call that caused it.
    const std::vector<result> dry = jcode::apply(r, root, true);

    reply going;

    bool stopped = false;

    for(const result& d : dry) {
        if(d.what == outcome::refused) {
            // A block can be refused *because* it had no usable name, and
            // "refused : why" reads like a formatting accident.
            say("refused " + (d.name.empty() ? std::string("a block") : d.name) +
                ": " + d.why);

            continue;
        }

        if(d.what == outcome::unchanged) {
            say(d.name + " is already what the model sent");

            continue;
        }

        // "would written" is what spell() gives, and it is not English. The
        // outcomes are named for what happened, which is the right name
        // everywhere except in front of "would".
        const char* verb = d.what == outcome::created ? "create" : "write";

        if(dry_run) {
            say(std::string("would ") + verb + " " + d.name + ", " + extent(d));

            continue;
        }

        // **Stop is not no.** No leaves this file and goes on asking; stop
        // leaves this one and every one after it, and asks nothing more --
        // which is what Escape at a dialog means, and why a person who has
        // seen enough is not made to refuse each remaining file in turn.
        if(!stopped && ask) {
            const consent c = ask(d);

            if(c == consent::stop) stopped = true;

            if(c != consent::yes) {
                say("left " + d.name + " alone");

                continue;
            }
        }
        else if(stopped) {
            say("left " + d.name + " alone");

            continue;
        }

        for(const edit& e : r.edits)
            if(e.name == d.name) going.edits.push_back(e);
    }

    if(going.edits.empty()) return false;

    bool wrote = false;

    for(const result& w : jcode::apply(going, root)) {
        // The size on the record, so a write that took more than it should
        // shows it even under --yes, where nobody was asked (#464).
        const bool landed = w.what == outcome::written ||
                            w.what == outcome::created;

        say(std::string(spell(w.what)) + " " + w.name +
            (landed ? ", " + extent(w) : std::string()) +
            (w.why.empty() ? std::string() : ": " + w.why));

        if(w.what == outcome::written || w.what == outcome::created)
            wrote = true;
    }

    return wrote;
}

}
}
}
