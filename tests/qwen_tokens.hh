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
#ifndef JLIB_TESTS_QWEN_TOKENS_HH
#define JLIB_TESTS_QWEN_TOKENS_HH

/**
 * Real tokenizations from the real Qwen 2.5 tokenizer.
 *
 * Produced by the HuggingFace `tokenizers` library reading Qwen 2.5's own
 * tokenizer.json, the same way llama3_tokens.hh was.
 *
 * ## The nfc flag
 *
 * Qwen's tokenizer.json carries `"normalizer": {"type": "NFC"}` and Llama
 * 3.2's carries none.  So the reference pipeline composes the text before it
 * cuts it, and `ai::tokenizer` does not: "e" followed by a combining acute
 * reaches the merges as two codepoints rather than one.
 *
 * The flagged cases are asserted **still different**, the way the `pretok`
 * cases in llama3_tokens.hh were before the pre-tokenizer existed.
 * Implementing NFC will make this test fail and say so, rather than passing
 * quietly with a fixture nobody re-read.
 *
 * NFC is the identity on ASCII, so this affects text carrying decomposed
 * sequences and nothing else -- which is why Qwen answers correctly with the
 * flag still set.
 */
struct qwen_tokenization {
    const char* text;
    int count;
    int ids[16];
    bool nfc;               ///< the reference normalises this and we do not
};
static const qwen_tokenization QWEN[] = {
    { "  double", 2, { 220, 1990 }, false },
    { "1234567890", 10, { 16, 17, 18, 19, 20, 21, 22, 23, 24, 15 }, false },
    { "   x", 2, { 256, 856 }, false },
    { "Hello world", 2, { 9707, 1879 }, false },
    { "don't", 2, { 15007, 944 }, false },
    { "DON'T", 2, { 84641, 17323 }, false },
    { "it's", 2, { 275, 594 }, false },
    { "The capital of France is Paris.", 7, { 785, 6722, 315, 9625, 374, 12095, 13 }, false },
    { "line1\nline2", 5, { 1056, 16, 198, 1056, 17 }, false },
    { "\360\237\216\211 emoji", 2, { 144841, 42365 }, false },
    { "\346\227\245\346\234\254\350\252\236", 2, { 101059, 102819 }, false },
    { "trailing ", 3, { 376, 14277, 220 }, false },
    { "!@#$%^&*()", 7, { 0, 31, 48077, 45899, 5, 9, 368 }, false },
    { "\t tab", 2, { 197, 5651 }, false },
    { "na\303\257ve caf\303\251", 4, { 3376, 37572, 586, 51950 }, false },
    { "a  b   c", 5, { 64, 220, 293, 256, 272 }, false },
    { "\n\n\n", 1, { 1406 }, false },
    { "  \n  ", 2, { 2303, 256 }, false },
    { "x\r\ny", 3, { 87, 319, 88 }, false },
    { "42", 2, { 19, 17 }, false },
    { "4242", 4, { 19, 17, 19, 17 }, false },
    { "424242", 6, { 19, 17, 19, 17, 19, 17 }, false },
    { "4242424242", 10, { 19, 17, 19, 17, 19, 17, 19, 17, 19, 17 }, false },
    { "\342\202\254100", 4, { 15056, 16, 15, 15 }, false },
    { "\302\275 cup", 2, { 26062, 10525 }, false },
    { "\342\205\243 chapter", 3, { 70467, 96, 12453 }, false },
    { "a1b2", 4, { 64, 16, 65, 17 }, false },
    { "  'tis", 4, { 220, 364, 83, 285 }, false },
    { "'x", 2, { 6, 87 }, false },
    { "\320\274\320\270\321\200", 1, { 130178 }, false },
    { "\330\247\331\204\330\271\330\261\330\250\331\212\330\251", 3, { 31382, 23224, 125600 }, false },
    { "\360\237\207\272\360\237\207\270 flag", 3, { 145526, 145383, 5181 }, false },
    { "e\314\201 combining", 2, { 963, 34171 }, true },
    { "Hello", 1, { 9707 }, false },
    { " Hello", 1, { 21927 }, false },
    { "a", 1, { 64 }, false },
    { " ", 1, { 220 }, false },
    { "\n", 1, { 198 }, false },
    { "capital", 1, { 65063 }, false },
    { " capital", 1, { 6722 }, false },
    { "France?", 2, { 49000, 30 }, false },
    { "<|im_start|>", 1, { 151644 }, false },
    { "a<|im_end|>b", 3, { 64, 151645, 65 }, false },
    { "The capital of France is Paris.", 7, { 785, 6722, 315, 9625, 374, 12095, 13 }, false },
};
static const std::size_t QWEN_COUNT = 44;

#endif // JLIB_TESTS_QWEN_TOKENS_HH
