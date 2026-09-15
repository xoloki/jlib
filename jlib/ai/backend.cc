/* -*- mode: C++ c-basic-offset: 4 -*-
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
 */

#include <jlib/ai/backend.hh>

namespace jlib {
namespace ai {

// The only two things here that do not depend on the element type.  Everything
// else is a template and lives in the header.

std::string name_of(activation a) {
    switch(a) {
    case activation::sigmoid:    return "sigmoid";
    case activation::tanh:       return "tanh";
    case activation::relu:       return "relu";
    case activation::leaky_relu: return "leaky_relu";
    case activation::silu:       return "silu";
    case activation::gelu:       return "gelu";
    }

    // Unreachable for any value of the enum, and kept because a compiler that
    // cannot see that still wants a return.  It used to be reachable: silu and
    // gelu fell through to here and were *named* sigmoid, which
    // activation_from_name would then refuse to read back.
    return "sigmoid";
}

activation activation_from_name(const std::string& s) {
    if(s == "sigmoid")    return activation::sigmoid;
    if(s == "tanh")       return activation::tanh;
    if(s == "relu")       return activation::relu;
    if(s == "leaky_relu") return activation::leaky_relu;
    if(s == "silu")       return activation::silu;
    if(s == "gelu")       return activation::gelu;

    throw backend_error("unknown activation '" + s + "'");
}

}
}
