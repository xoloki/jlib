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
#ifndef JLIB_TESTS_GEMMA_TOKENS_HH
#define JLIB_TESTS_GEMMA_TOKENS_HH

/**
 * Real tokenizations from the real Gemma 2 tokenizer.
 *
 * From the HuggingFace `tokenizers` library reading Gemma 2's own
 * tokenizer.json, the same way llama3_tokens.hh and qwen_tokens.hh were.
 *
 * ## Why this file is the interesting one
 *
 * Gemma 2 is a `llama` vocabulary like TinyLlama's, and the two are driven the
 * **opposite** ways -- which is the whole reason `tokenizer::driver` exists.
 *
 *     TinyLlama   61249 merges, and all 32000 scores are zero
 *     Gemma 2     no merges at all, and 256000 real scores
 *
 * Gemma's scores are ranks rather than log-probabilities: `-(index - 473)`
 * across the ordinary vocabulary, the offset being the specials and byte
 * tokens ahead of it. So the best-scoring pair is the earliest one in the
 * vocabulary.
 *
 * It also sets `add_space_prefix` to 0, so `Hello` is 4521 and ` Hello` is
 * 25957 -- two different tokens, where on Llama both come out as the marked
 * form. These fixtures are what caught that: without the flag every one of
 * them was off by a marker.
 */
static const tokenization GEMMA[] = {
    { "Hello", 1, { 4521 } },
    { " Hello", 1, { 25957 } },
    { "Hello world", 2, { 4521, 2134 } },
    { "a", 1, { 235250 } },
    { " ", 1, { 235248 } },
    { "\n", 1, { 108 } },
    { "capital", 1, { 37301 } },
    { " capital", 1, { 6037 } },
    { "The capital of France is Paris.", 7, { 651, 6037, 576, 6081, 603, 7127, 235265 } },
    { "France?", 2, { 21456, 235336 } },
    { "  double", 2, { 139, 4576 } },
    { "\t tab", 2, { 226, 6684 } },
    { "na\303\257ve caf\303\251", 4, { 556, 236370, 524, 18688 } },
    { "\346\227\245\346\234\254\350\252\236", 1, { 62938 } },
    { "\360\237\216\211 emoji", 2, { 239548, 52810 } },
    { "line1\nline2", 5, { 703, 235274, 108, 703, 235284 } },
    { "trailing ", 2, { 100504, 235248 } },
    { "1234567890", 10, { 235274, 235284, 235304, 235310, 235308, 235318, 235324, 235321, 235315, 235276 } },
    { "  ", 1, { 139 } },
    { "   x", 2, { 140, 235297 } },
    { "don't", 3, { 7589, 235303, 235251 } },
    { "!@#$%^&*()", 7, { 235341, 235348, 77881, 214207, 235343, 235287, 645 } },
    { "<eos>", 1, { 1 } },
    { "a<eos>b", 3, { 235250, 1, 235268 } },
    { "<start_of_turn>", 1, { 106 } },
    { "\320\274\320\270\321\200", 1, { 39572 } },
    { "\342\202\254100", 4, { 235872, 235274, 235276, 235276 } },
    { "a1b2", 4, { 235250, 235274, 235268, 235284 } },
};
static const std::size_t GEMMA_COUNT = 28;

#endif // JLIB_TESTS_GEMMA_TOKENS_HH
