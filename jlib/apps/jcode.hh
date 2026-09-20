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
 * What to tell a model so that parse() can read what comes back.
 *
 * **Beside the parser on purpose.** The prompt and the parser are two halves
 * of one contract -- this says "filename, fence, whole file, fence" and
 * parse() is what happens when the model does something else. Put them in
 * different files and they drift, and the drift shows up as a harness that
 * quietly stops applying edits.
 */
std::string system_prompt();

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
             std::size_t budget);

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
