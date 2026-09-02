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
#ifndef JLIB_TESTS_KQUANT_VALUES_HH
#define JLIB_TESTS_KQUANT_VALUES_HH

/**
 * Real dequantised weights from a real K-quantised file.
 *
 * From the official `gguf` Python package's `dequantize()` reading
 * gemma-2-2b-it-Q4_K_M.gguf -- not from this code, and not by hand. A
 * dequantiser is exactly the kind of thing that can look right and be wrong
 * in the fourth decimal, and round-tripping cannot catch it because there is
 * nothing to round-trip against.
 *
 * The two tensors are chosen because Q4_K_M is a **mixture** -- `ffn_gate` is
 * q4_K and `ffn_down` is q6_K in the same file -- so one file exercises both
 * formats, which is also why reading such a file needs both.
 *
 * The expected values are exact, not approximate: dequantisation is integer
 * arithmetic and two f16 multiplies, so a correct implementation reproduces
 * them bit for bit. Any tolerance here would be hiding something.
 */
struct kquant_case {
    const char* tensor;
    const char* type;
    int count;
    float first[64];
};

static const kquant_case KQUANT[] = {
    { "blk.0.ffn_gate.weight", "q4_K", 64, {
        0.00217151642f, -0.0180094242f, -0.00455546379f, -0.0180094242f,
        -0.011282444f, 0.00665616989f, 0.00217151642f, -0.00679779053f,
        -0.0135247707f, -0.00231313705f, -0.011282444f, -7.0810318e-05f,
        0.0156254768f, -0.00231313705f, 0.00217151642f, 0.00889849663f,
        0.0133831501f, -0.00231313705f, 0.00217151642f, -7.0810318e-05f,
        -0.0135247707f, -0.00904011726f, 0.00217151642f, -0.00904011726f,
        -0.011282444f, 0.00889849663f, -0.00904011726f, -0.00231313705f,
        -0.00904011726f, 0.00217151642f, -7.0810318e-05f, 0.00441384315f,
        -0.0128638744f, 0.0134834647f, 0.0117269754f, 0.00470101833f,
        0.00821399689f, -0.00583791733f, 0.00118803978f, -0.0111073852f,
        -0.000568449497f, 0.00118803978f, -0.00583791733f, -0.00232493877f,
        -0.00408142805f, -0.00408142805f, 0.00645750761f, 0.00645750761f,
        0.00470101833f, 0.00294452906f, -0.000568449497f, -0.0111073852f,
        0.00294452906f, -0.00408142805f, -0.0111073852f, 0.00821399689f,
        0.00470101833f, -0.00232493877f, 0.00118803978f, -0.00232493877f,
        0.00118803978f, -0.000568449497f, 0.00294452906f, 0.00821399689f,
    } },
    { "blk.0.ffn_down.weight", "q6_K", 64, {
        -0.000424861908f, 0.0135955811f, 0.00212430954f, -0.0f,
        0.00594806671f, 0.00594806671f, 0.00297403336f, -0.0114712715f,
        0.0050983429f, -0.00212430954f, 0.00467348099f, -0.0118961334f,
        0.00424861908f, 0.0127458572f, -0.00424861908f, 0.00807237625f,
        0.00175094604f, 0.0039396286f, -0.00437736511f, -0.00218868256f,
        0.00831699371f, 0.00919246674f, -0.00437736511f, 0.00831699371f,
        -0.00350189209f, 0.000875473022f, 0.000875473022f, 0.0109434128f,
        -0.0100679398f, -0.00437736511f, 0.0135698318f, 0.00481510162f,
        -0.0f, -0.00387525558f, -0.00830411911f, 0.0116257668f,
        0.00221443176f, -0.00498247147f, 0.00332164764f, -0.00553607941f,
        0.00442886353f, 0.00775051117f, -0.0027680397f, -0.0149474144f,
        0.0177154541f, -0.00885772705f, 0.00553607941f, 0.00332164764f,
        0.00148701668f, -0.015365839f, -0.0104091167f, 0.0f,
        0.0148701668f, 0.00892210007f, 0.000991344452f, 0.0128874779f,
        -0.00842642784f, 0.00396537781f, 0.00495672226f, 0.00644373894f,
        -0.00594806671f, -0.00693941116f, 0.00396537781f, -0.00297403336f,
    } },
};

#endif // JLIB_TESTS_KQUANT_VALUES_HH
