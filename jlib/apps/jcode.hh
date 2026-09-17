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

#include <string>
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
