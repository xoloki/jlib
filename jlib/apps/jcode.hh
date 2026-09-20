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

#ifndef JLIB_APPS_JCODE_HH
#define JLIB_APPS_JCODE_HH

/**
 * @file
 *
 * What a model said, turned into files on disk (#265).
 *
 * The format is aider's `whole`: a filename on its own line, a fence, the
 * entire file, a closing fence. It needs no tool calling, which is why #242
 * starts here.
 *
 *     path/to/filename.js
 *     ```
 *     // entire file content
 *     ```
 *
 * ## Lenient, and every guess on the record
 *
 * Models get the envelope wrong constantly -- they bold the filename, end it
 * with a colon, make it a heading, wrap it in backticks, or paste the
 * `path/to` prefix out of the prompt's own example. A harness that refuses a
 * reply over any of that is useless, so each of those is absorbed.
 *
 * **Each one is a guess, and each is recorded against the edit it produced.**
 * A guess nobody can see is how the wrong file gets written and nobody learns
 * why. `edit::guesses` carries them; a caller is expected to print them.
 *
 * Anything not on that list is refused rather than guessed at, so the list is
 * the contract and grows only with a failure to point at.
 */

#include <jlib/ai/openai.hh>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace jlib {
namespace apps {
namespace jcode {

/** One file the model asked for, and what was assumed to get it. */
struct edit {
    /** The name after cleaning.  Relative; see contain(). */
    std::string name;

    /** The whole file, as the model gave it. */
    std::string content;

    /**
     * What was absorbed to arrive at that name, in the order it happened.
     *
     * Empty is the good case and the common one.  Anything here means the
     * reply was not in the format asked for and jcode decided what it meant.
     */
    std::vector<std::string> guesses;
};

/** A block that was not taken, and why not. */
struct refusal {
    std::string name;      ///< empty when that was the problem
    std::string why;
};

/** What a reply turned out to contain. */
struct reply {
    std::vector<edit> edits;
    std::vector<refusal> refusals;

    /**
     * Calls the model asked for, which jcode cannot make yet (#316).
     *
     * **Recorded rather than ignored.** The markup is prose to the edit
     * parser, so a reply that is entirely a tool call reads as a reply with
     * no edits in it -- and "the model produced nothing usable" is what a
     * model too weak to follow the format looks like. These are different
     * problems and a user has no way to tell them apart from silence.
     *
     * Acting on one is the agent loop, which is #310's second piece.
     */
    std::vector<ai::openai::call> calls;

    /** Every guess across every edit, for a caller that just wants to print. */
    std::vector<std::string> guesses() const;
};

/**
 * Read a model's reply.
 *
 * @param text  the whole reply
 * @param known the files that were sent, which is what makes the basename
 *              rescue and "is this a new file" answerable.  May be empty.
 *
 * Never throws and never writes anything: a reply is data, and the decision to
 * put it on disk is apply()'s.
 *
 * **A block whose fence never closes is refused, always.** jserve stops at the
 * context wall and a whole-file reply cut short is a corrupted file -- the
 * same hazard #256 is about, arriving from the other side. That refusal is the
 * load-bearing one here.
 *
 * A file containing a fence line cannot survive this format at all: the block
 * ends early and what follows reads as prose. aider chooses a fence that does
 * not collide with the files it sends and records that longer fences do not
 * help, because models revert to three backticks anyway. That is a property of
 * the format rather than a defect here, and the response is to notice rather
 * than to write half a file.
 */
reply parse(const std::string& text, const std::vector<std::string>& known);

// ---------------------------------------------------------------- the prompt

/**
 * Something the model may ask for, and what happens when it does.
 *
 * `parameters` is a JSON Schema as text, for the same reason
 * `openai::request::tools` is: the shape belongs to whoever wrote the tool and
 * there is no struct that holds one without losing something.
 *
 * `run` is given the call's arguments as JSON text and returns what the model
 * is shown. It may throw: the loop turns that into a result saying so, because
 * a model waiting for an answer it will never get is worse than a model told
 * the tool failed.
 */
struct tool {
    std::string name;
    std::string description;
    std::string parameters;

    std::function<std::string(const std::string& arguments)> run;
};

/**
 * What to tell a model so that parse() can read what comes back.
 *
 * **Beside the parser on purpose.** The prompt and the parser are two halves
 * of one contract -- this says "filename, fence, whole file, fence" and
 * parse() is what happens when the model does something else. Put them in
 * different files and they drift, and the drift shows up as a harness that
 * quietly stops applying edits.
 */
std::string system_prompt(const std::vector<tool>& tools =
                              std::vector<tool>());

/**
 * How many tokens a string is likely to be, erring high.
 *
 * jcode has no tokenizer -- the vocabulary is behind the server -- so this is
 * a guess, and the direction of the error is chosen rather than accidental.
 *
 * Measured over eight files of jlib source and prose, bytes per token:
 *
 *     Qwen2.5-Coder   3.84 overall, 3.52 worst
 *     Llama 3.2       3.87 overall, 3.53 worst
 *     TinyLlama       3.12 overall, 2.80 worst
 *
 * The folk rule of four is wrong in the direction that costs: it undercounts
 * by 4% on the coder vocabularies and 28% on TinyLlama's older 32k one, and
 * undercounting means overflowing a context you believed you fitted -- which
 * is #256's silent trim, arriving at the client that cannot see it.
 *
 * So the divisor is the **worst** ratio seen, not the mean. Sending fewer
 * files than strictly necessary costs a little context; sending more than fits
 * costs the head of the conversation without saying so.
 */
std::size_t estimate_tokens(const std::string& text);

/** The divisor above, exposed so a caller can say what it used. */
double bytes_per_token();

/** One file to put in front of the model. */
struct source {
    std::string name;
    std::string content;
};

/** A request laid out, and everything decided on the way. */
struct plan {
    /** The turns to send, system first. */
    std::vector<std::pair<std::string, std::string> > turns;

    /** The files that made it in, in the order they appear. */
    std::vector<std::string> sent;

    /** What was left out, and why -- the same policy as every other guess. */
    std::vector<std::string> dropped;

    /** What estimate_tokens() made of the whole prompt. */
    std::size_t estimate = 0;
};

/**
 * Lay out a request that fits `budget` tokens.
 *
 * Files are dropped **whole**, never truncated: half a file in a prompt is
 * worse than no file, because the model will complete it rather than notice.
 * They go in the order given and the ones that do not fit are the ones at the
 * end, which is a rule a caller can see rather than a relevance judgement
 * nothing here is qualified to make.
 *
 * The newest thing -- the request itself -- is never dropped. A request whose
 * own question does not fit is laid out anyway and left to the server to
 * refuse, because refusing it here would be this code deciding what the model
 * can read.
 */
plan lay_out(const std::string& request, const std::vector<source>& files,
             std::size_t budget,
             const std::vector<tool>& tools = std::vector<tool>());

/**
 * How far the estimate was out, once a reply says what it really cost.
 *
 * @param estimated what plan::estimate said
 * @param actual    usage.prompt_tokens from the reply
 *
 * An estimate that reports its own error is a different thing from a
 * constant, and this is the only place the true ratio for a given model is
 * ever visible: the vocabulary is on the other side of the wire.
 */
std::string estimate_drift(std::size_t estimated, std::size_t actual);

/** The tool list as the protocol wants it, for `openai::request::tools`. */
std::string declare(const std::vector<tool>& tools);

/**
 * Why a conversation stopped.
 *
 * Four outcomes rather than a bool, because a harness that exits the same way
 * for all of them is telling the user nothing -- which is the failure this
 * whole arc keeps finding in different clothes.
 */
enum class ending {
    answered,    ///< the model replied without asking for anything
    bounded,     ///< the round limit was reached, and it had more to say
    refused,     ///< the far end refused; see detail
    failed       ///< the exchange threw; see detail
};

const char* spell(ending e);

/** One call the loop dispatched, for announcing and for tests. */
struct ran {
    std::string name;
    std::string arguments;
    bool known = false;    ///< false when no tool by that name is registered
    std::string failed;    ///< set when the handler threw
};

/** How a conversation ended, and everything it did on the way. */
struct conversation {
    std::vector<ai::message> turns;   ///< the whole exchange, as it ended
    std::string text;                 ///< the final reply's prose

    ending why = ending::answered;
    std::string detail;

    unsigned int rounds = 0;
    std::vector<ran> calls;
};

/**
 * One exchange with the far end: the turns and the tool list in, a reply out.
 *
 * Injected rather than called directly so the loop can be tested with no
 * model, no server and no disk -- which is most of why it is here rather than
 * in jcode_main.
 */
typedef std::function<ai::openai::answer(const std::vector<ai::message>&,
                                         const std::string& tools)> exchange;

/**
 * Drive a conversation until the model stops asking for tools.
 *
 * Each round: send, and if the reply carries calls, run them and go round with
 * the results appended. **The assistant turn carries the calls themselves**,
 * not only the results -- `ai::chat` renders them into the model's own markup
 * (#311), and without them the model is shown a result for a call it cannot
 * see itself having made, and asks again.
 *
 * A call naming a tool that is not registered is refused, and the refusal is
 * sent back as that call's result. A model that asked for something which does
 * not exist should be told so rather than left waiting -- and the refusal is
 * recorded in `conversation::calls` rather than hidden, which is jcode's rule
 * from #265 applied to a new kind of guess.
 *
 * @param max_rounds a bound, because a model that calls the same tool forever
 *        is the ordinary failure of a loop like this rather than an exotic one
 *
 * Never writes anything. Applying the edits in the final reply is still
 * apply()'s, and still the caller's decision.
 */
conversation converse(std::vector<ai::message> turns,
                      const std::vector<tool>& tools,
                      const exchange& send,
                      unsigned int max_rounds = 8);

/**
 * The tools jcode offers, bounded by `root`.
 *
 * ## What each one may do
 *
 * - **`read_file`** returns a file's contents. #269 answered which files fit;
 *   this answers which are *relevant*, which it could not ask before -- a
 *   model needing a header it was not handed had no way to say so.
 * - **`search`** finds a string in the tree, and answers the same question one
 *   step earlier: which files are worth reading at all.
 * - **`build`** runs the command named in `build`, and **is not registered at
 *   all when that is empty**. A jcode given no `--build` cannot run anything.
 *
 * ## What bounds them
 *
 * **Containment is by resolution, not inspection.** Every path is resolved
 * with `realpath` and compared against the resolved root, which is the same
 * mechanism apply() uses for writes -- so `..`, an absolute path and a symlink
 * pointing out are one refusal rather than three checks that happen to agree.
 *
 * **No shell.** `build` is split on whitespace and run through
 * `sys::run(argv, ...)`, which starts a program with no `/bin/sh` involved.
 * So `&&`, `|` and `>` do not work, deliberately: `sys.hh` records that jlib
 * had five `shell()` callers interpolating strings they did not choose, two of
 * them `rm` and `mv` on a folder name from a mail server. A build that needs a
 * shell needs a script, and the script is what gets named.
 *
 * **Every result is capped**, and says so when it was cut. A tool result goes
 * into the next prompt, so an unbounded one is a context wall reached by
 * accident -- and a silently shortened file is worse than a refusal, because
 * the model believes it read the whole thing.
 *
 * ## What is deliberately absent
 *
 * **Nothing here writes.** Editing stays where it is: the model answers with
 * whole files and apply() puts them on disk, announced and confirmed. A
 * `write_file` tool would move that decision inside the loop where nobody
 * sees it, which is exactly the erosion #242 names and this arc is under
 * instruction not to perform quietly.
 *
 * @param root  what every path is relative to, and cannot escape
 * @param build the build command, split on whitespace; empty for none
 * @param cap   the most bytes any one result may carry
 */
std::vector<tool> toolbox(const std::string& root,
                          const std::string& build = std::string(),
                          std::size_t cap = 64 * 1024);

/** What became of one edit. */
enum class outcome {
    written,     ///< the file on disk now holds what the model said
    unchanged,   ///< it already did, byte for byte -- nothing was done
    created,     ///< it did not exist before
    refused      ///< see the reason
};

const char* spell(outcome o);

struct result {
    std::string name;
    outcome what = outcome::refused;
    std::string why;              ///< set when refused
    std::vector<std::string> guesses;
};

/**
 * Put the edits on disk under `root`.
 *
 * @param dry_run decide everything and write nothing
 *
 * **`unchanged` is not `written`.** A model that returns the file it was given
 * has changed nothing, and reporting "applied" for it is the harness lie this
 * arc keeps finding -- so it is its own outcome and the caller can tell.
 * `created` is separate for the same reason: a name matching nothing that was
 * sent is either a new file or a hallucination, and only the caller knows
 * which.
 *
 * **Containment is by resolution, not inspection.** The name came from a
 * model, which makes it untrusted by construction. The parent directory is
 * resolved with realpath and compared against the resolved root, so `..`,
 * a symlink out, and an absolute path are all the same refusal -- and a
 * case-insensitive filesystem cannot be talked into disagreeing with the check
 * the way #262's bypass did, because the check is the resolution.
 */
std::vector<result> apply(const reply& r, const std::string& root,
                          bool dry_run = false);

}
}
}

#endif // JLIB_APPS_JCODE_HH
