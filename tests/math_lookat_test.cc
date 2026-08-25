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

// math::matrix::lookAt.
// lookAt actually a look-at?
#include <jlib/math/matrix.hh>
#include <cmath>
#include <iomanip>
#include <iostream>
using namespace jlib::math;
typedef double T;

static int failures = 0;
static void check(const char* what, double got, double want, double tol = 1e-12) {
    const bool ok = std::fabs(got - want) <= tol;
    if(!ok) ++failures;
    std::cout << (ok ? "  ok   " : "  FAIL ") << std::setw(42) << std::left << what
              << std::scientific << std::setprecision(2) << got
              << "  (want " << want << ")\n";
}

int main() {
    // 1. the apps' old call vs what replaced it: translate(-eye)
    std::cout << "apps: old lookAt(-eye only) against translate(-eye)\n";
    for(uint n = 4; n <= 7; n++) {
        vertex<T> eye(n), back(n);
        for(uint i = 0; i < n; i++) { eye[i] = 0; back[i] = 0; }
        for(uint i = 3; i < n; i++) { eye[i] = 3; back[i] = -3; }

        matrix<T> t = matrix<T>::translate(n, back);

        // the old lookAt was exactly this, so a sample point must agree
        vertex<T> v(n), out(n);
        for(uint i = 0; i < n; i++) v[i] = 0.5 + i;
        out = t * v();

        T worst = 0;
        for(uint i = 0; i < n; i++)
            worst = std::max(worst, std::fabs(out[i] - (v[i] + back[i])));

        char buf[64]; std::snprintf(buf, sizeof(buf), "D=%u translate matches -eye", n);
        check(buf, worst, 0.0);
    }

    // 2. the new lookAt
    std::cout << "\nlookAt as a camera\n";
    for(uint n = 3; n <= 6; n++) {
        vertex<T> eye(n), up(n), center(n);
        for(uint i = 0; i < n; i++) { eye[i] = 0; up[i] = 0; center[i] = 0; }
        eye[0] = 2; eye[1] = 3; eye[2] = 5;          // off-axis on purpose
        if(n > 3) eye[3] = -1;
        up[1] = 1;
        center[0] = 1;                                 // and not at the origin

        matrix<T> m = matrix<T>::lookAt(n, eye, up, center);

        // the eye lands on the origin
        vertex<T> o(n); o = m * eye();
        T eye_off = 0;
        for(uint i = 0; i < n; i++) eye_off = std::max(eye_off, std::fabs(o[i]));
        char b1[64]; std::snprintf(b1, sizeof(b1), "n=%u eye maps to the origin", n);
        check(b1, eye_off, 0.0, 1e-12);

        // the centre lands on the negative last axis and nowhere else
        vertex<T> c(n); c = m * center();
        T off_axis = 0;
        for(uint i = 0; i + 1 < n; i++) off_axis = std::max(off_axis, std::fabs(c[i]));
        char b2[64]; std::snprintf(b2, sizeof(b2), "n=%u centre is on one axis", n);
        check(b2, off_axis, 0.0, 1e-12);
        char b3[64]; std::snprintf(b3, sizeof(b3), "n=%u centre is in front (negative)", n);
        check(b3, c[n-1] < 0 ? 1.0 : 0.0, 1.0);

        // the basis is orthonormal: rows dotted with each other give identity
        T worst = 0;
        for(uint i = 0; i < n; i++) {
            for(uint j = 0; j < n; j++) {
                T dot = 0;
                for(uint k = 0; k < n; k++) dot += m(i,k) * m(j,k);
                worst = std::max(worst, std::fabs(dot - (i == j ? 1.0 : 0.0)));
            }
        }
        char b4[64]; std::snprintf(b4, sizeof(b4), "n=%u basis is orthonormal", n);
        check(b4, worst, 0.0, 1e-12);

        // up, with the forward part taken out, points along +e_1
        vertex<T> u(n), ru(n);
        for(uint i = 0; i < n; i++) u[i] = up[i] + eye[i];   // a point, not a direction
        ru = m * u();
        char b5[64]; std::snprintf(b5, sizeof(b5), "n=%u up maps to +e_1", n);
        check(b5, ru[1] > 0 ? 1.0 : 0.0, 1.0);
    }

    return failures ? 1 : 0;
}
