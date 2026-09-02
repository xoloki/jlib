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
#ifndef JLIB_TESTS_QWEN_SPLITS_HH
#define JLIB_TESTS_QWEN_SPLITS_HH

/**
 * The qwen2 pre-tokenizer's answers, from the pattern itself.
 *
 * Qwen 2.5 names `qwen2` in `tokenizer.ggml.pre`, and that pattern differs
 * from `llama-bpe` in **one character**: `\p{N}` where Llama has
 * `\p{N}{1,3}`, so a digit is its own chunk and "1234567890" is ten rather
 * than four.  Everything else in the two is the same text.
 *
 * Produced by Python's `regex` module running the pattern read straight out
 * of Qwen 2.5's tokenizer.json -- not typed in from the header, so if the
 * grammar and the pattern ever disagree this says so.
 *
 * The same 34 inputs as llama3_splits.hh, deliberately: it is the *contrast*
 * that is worth having in the tree, and eight of them cut differently.
 */
static const pre_split QWEN_SPLITS[] = {
    { "  double", 2, { " ", " double" } },
    { "1234567890", 10, { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" } },
    { "   x", 2, { "  ", " x" } },
    { "Hello world", 2, { "Hello", " world" } },
    { "don't", 2, { "don", "'t" } },
    { "DON'T", 2, { "DON", "'T" } },
    { "it's", 2, { "it", "'s" } },
    { "The capital of France is Paris.", 7, { "The", " capital", " of", " France", " is", " Paris", "." } },
    { "line1\nline2", 5, { "line", "1", "\n", "line", "2" } },
    { "\360\237\216\211 emoji", 2, { "\360\237\216\211", " emoji" } },
    { "\346\227\245\346\234\254\350\252\236", 1, { "\346\227\245\346\234\254\350\252\236" } },
    { "trailing ", 2, { "trailing", " " } },
    { "!@#$%^&*()", 1, { "!@#$%^&*()" } },
    { "\t tab", 2, { "\t", " tab" } },
    { "na\303\257ve caf\303\251", 2, { "na\303\257ve", " caf\303\251" } },
    { "a  b   c", 5, { "a", " ", " b", "  ", " c" } },
    { "\n\n\n", 1, { "\n\n\n" } },
    { "  \n  ", 2, { "  \n", "  " } },
    { "x\r\ny", 3, { "x", "\r\n", "y" } },
    { "42", 2, { "4", "2" } },
    { "4242", 4, { "4", "2", "4", "2" } },
    { "424242", 6, { "4", "2", "4", "2", "4", "2" } },
    { "4242424242", 10, { "4", "2", "4", "2", "4", "2", "4", "2", "4", "2" } },
    { "\342\202\254100", 4, { "\342\202\254", "1", "0", "0" } },
    { "\302\275 cup", 2, { "\302\275", " cup" } },
    { "\342\205\243 chapter", 2, { "\342\205\243", " chapter" } },
    { "a1b2", 4, { "a", "1", "b", "2" } },
    { "  'tis", 3, { " ", " '", "tis" } },
    { "'x", 1, { "'x" } },
    { "", 0, { nullptr } },
    { "\320\274\320\270\321\200", 1, { "\320\274\320\270\321\200" } },
    { "\330\247\331\204\330\271\330\261\330\250\331\212\330\251", 1, { "\330\247\331\204\330\271\330\261\330\250\331\212\330\251" } },
    { "\360\237\207\272\360\237\207\270 flag", 2, { "\360\237\207\272\360\237\207\270", " flag" } },
    { "e\314\201 combining", 3, { "e", "\314\201", " combining" } },
};

#endif // JLIB_TESTS_QWEN_SPLITS_HH
