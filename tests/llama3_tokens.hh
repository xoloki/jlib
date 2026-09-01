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

#ifndef JLIB_TESTS_LLAMA3_TOKENS_HH
#define JLIB_TESTS_LLAMA3_TOKENS_HH

/**
 * Real tokenizations from the real Llama 3 tokenizer.
 *
 * Produced by the HuggingFace `tokenizers` library reading Llama 3.2's own
 * tokenizer.json -- not by this code, and not by hand.  Recognising that 9906
 * "looks like" the id for Hello is not evidence, and this file exists because
 * that is the sort of claim that turns out wrong.
 *
 * `pretok` marks a case whose answer depends on pre-tokenization -- the regex
 * that splits text into chunks before the merges run, which this tokenizer
 * does not implement yet.  They are listed rather than omitted: a known
 * difference that is written down is a task, and one that is quietly excluded
 * from the fixtures is a bug nobody will find.
 */
struct tokenization {
    const char* text;
    int count;
    int ids[16];
    bool pretok;
};

static const tokenization LLAMA3[] = {
    { "Hello", 1, { 9906 }, false },
    { " Hello", 1, { 22691 }, false },
    { "Hello world", 2, { 9906, 1917 }, false },
    { "a", 1, { 64 }, false },
    { " ", 1, { 220 }, false },
    { "\n", 1, { 198 }, false },
    { "capital", 1, { 66163 }, false },
    { " capital", 1, { 6864 }, false },
    { "France?", 2, { 50100, 30 }, false },
    { "  double", 2, { 220, 2033 }, true },
    { "\t tab", 2, { 197, 5769 }, false },
    { "na\303\257ve caf\303\251", 4, { 3458, 38672, 588, 53050 }, false },
    { "\346\227\245\346\234\254\350\252\236", 2, { 102433, 102158 }, false },
    { "\360\237\216\211 emoji", 4, { 9468, 236, 231, 43465 }, false },
    { "The capital of France is Paris.", 7, { 791, 6864, 315, 9822, 374, 12366, 13 }, false },
    { "line1\nline2", 5, { 1074, 16, 198, 1074, 17 }, false },
    { "trailing ", 3, { 376, 14612, 220 }, false },
    { "<|eot_id|>", 1, { 128009 }, false },
    { "a<|eot_id|>b", 3, { 64, 128009, 65 }, false },
    { "!@#$%^&*()", 7, { 0, 31, 49177, 46999, 5, 9, 368 }, false },
    { "1234567890", 4, { 4513, 10961, 16474, 15 }, true },
    { "  ", 1, { 256 }, false },
    { "   x", 2, { 256, 865 }, true },
};

static const std::size_t LLAMA3_COUNT = sizeof(LLAMA3) / sizeof(LLAMA3[0]);

#endif // JLIB_TESTS_LLAMA3_TOKENS_HH
