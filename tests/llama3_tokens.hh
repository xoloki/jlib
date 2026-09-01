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
 * Three of these once carried a `pretok` flag, marking answers that depended
 * on pre-tokenization -- the regex that splits text into chunks before the
 * merges run.  The test asserted they were *still wrong*, so that implementing
 * the pre-tokenizer would make it fail and say so rather than passing quietly
 * with a fixture nobody re-read.  It did exactly that, and the flag is gone
 * because nothing is pending any more.
 */
struct tokenization {
    const char* text;
    int count;
    int ids[16];
};

static const tokenization LLAMA3[] = {
    { "Hello", 1, { 9906 } },
    { " Hello", 1, { 22691 } },
    { "Hello world", 2, { 9906, 1917 } },
    { "a", 1, { 64 } },
    { " ", 1, { 220 } },
    { "\n", 1, { 198 } },
    { "capital", 1, { 66163 } },
    { " capital", 1, { 6864 } },
    { "France?", 2, { 50100, 30 } },
    { "  double", 2, { 220, 2033 } },
    { "\t tab", 2, { 197, 5769 } },
    { "na\303\257ve caf\303\251", 4, { 3458, 38672, 588, 53050 } },
    { "\346\227\245\346\234\254\350\252\236", 2, { 102433, 102158 } },
    { "\360\237\216\211 emoji", 4, { 9468, 236, 231, 43465 } },
    { "The capital of France is Paris.", 7, { 791, 6864, 315, 9822, 374, 12366, 13 } },
    { "line1\nline2", 5, { 1074, 16, 198, 1074, 17 } },
    { "trailing ", 3, { 376, 14612, 220 } },
    { "<|eot_id|>", 1, { 128009 } },
    { "a<|eot_id|>b", 3, { 64, 128009, 65 } },
    { "!@#$%^&*()", 7, { 0, 31, 49177, 46999, 5, 9, 368 } },
    { "1234567890", 4, { 4513, 10961, 16474, 15 } },
    { "  ", 1, { 256 } },
    { "   x", 2, { 256, 865 } },
};

static const std::size_t LLAMA3_COUNT = sizeof(LLAMA3) / sizeof(LLAMA3[0]);

#endif // JLIB_TESTS_LLAMA3_TOKENS_HH
