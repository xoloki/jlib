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

/**
 * Is jhardhyper's D-to-3 reduction worth moving to the GPU?
 *
 * The CPU path here is transcribed from HPlot<T>::transform in
 * jlib/apps/jhardhyper.cc -- deliberately the same lines rather than a
 * tidier rewrite, because a benchmark that measures a different computation
 * answers a different question.  The GPU path is metal::hyper_reduce.
 *
 * What it prints per dimension: the per-frame cost of each, the ratio, and
 * the worst componentwise divergence between the GPU's floats and the CPU's
 * doubles.  The last column is the one that can sink the whole idea
 * regardless of the timings, so it is measured rather than assumed.
 *
 * The vertex count follows jhardhyper's hypercube, 2^D, which is why the
 * work grows so steeply: the per-vertex chain is O(D^3) and there are 2^D of
 * them.  That is the effect the frame-rate cliff is made of.
 */

#include <jlib/metal/hyper_reduce.hh>
#include <jlib/math/matrix.hh>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace jlib;

namespace {

typedef std::chrono::steady_clock clk;

double micros(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double, std::micro>(b - a).count();
}

/**
 * Points on the unit sphere in D dimensions, deterministically.
 *
 * Normalised gaussians, which is the one construction that is actually
 * uniform on the sphere.  The engine is specified by the standard and the
 * distributions are not -- libc++ and libstdc++ map the same mt19937_64
 * sequence differently -- so the mapping is done here by hand to keep the
 * figure identical across platforms.
 */
std::vector<double> sphere(unsigned d, std::size_t n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);

    const unsigned w = d + 1;

    std::vector<double> ret(n * w);

    for(std::size_t i = 0; i < n; i++) {
        double r2 = 0;

        for(unsigned k = 0; k < d; k++) {
            // Box-Muller from two hand-mapped uniforms in (0,1].
            const double u1 = double((rng() >> 11) + 1) / 9007199254740993.0;
            const double u2 = double((rng() >> 11) + 1) / 9007199254740993.0;

            const double g = std::sqrt(-2 * std::log(u1)) * std::cos(2 * M_PI * u2);

            ret[i * w + k] = g;
            r2 += g * g;
        }

        const double r = std::sqrt(r2) > 1e-12 ? std::sqrt(r2) : 1;

        for(unsigned k = 0; k < d; k++)
            ret[i * w + k] /= r;

        ret[i * w + d] = 1;
    }

    return ret;
}

/** jhardhyper's clip volume; see initialize_glazzies. */
std::vector< std::pair<double,double> > clip_of(unsigned d) {
    const double r22 = 1.5;

    std::vector< std::pair<double,double> > clip;

    clip.push_back(std::make_pair(-r22, r22));
    clip.push_back(std::make_pair(-r22, r22));

    if(d > 2)
        clip.push_back(std::make_pair(-r22, r22));

    // Every axis above the third is projected away and needs a frustum
    // wholly in front of the eye.
    for(unsigned i = 3; i < d; i++)
        clip.push_back(std::make_pair(r22, 3 * r22));

    return clip;
}

/**
 * The modelview jhardhyper would have at rest, near enough.
 *
 * Identity with the projected-away axes pushed in front of their frustum.
 * project() yields w = -x_(d-1), so the offset is negative: a positive one
 * puts every vertex behind the eye and the divide mirrors the figure through
 * the origin, which is the bug the clip comment in jhardhyper records.
 */
math::matrix<double> modelview_of(unsigned d) {
    math::matrix<double> mv = math::matrix<double>::identity(d + 1);

    for(unsigned i = 3; i < d; i++)
        mv(i, d) = -3.0;

    return mv;
}

/** Row-major flatten, which is the layout hyper_reduce wants. */
std::vector<double> flatten(const math::matrix<double>& m, unsigned side) {
    std::vector<double> ret(std::size_t(side) * side);

    for(unsigned i = 0; i < side; i++)
        for(unsigned j = 0; j < side; j++)
            ret[std::size_t(i) * side + j] = m(i, j);

    return ret;
}

/**
 * The CPU reduction, transcribed from HPlot<T>::transform.
 *
 * Stereographic is left out: it is one divide before the loop and off by
 * default, and including it would mean reproducing the percentile pass too.
 * Returns n*4 doubles.
 */
std::vector<double> cpu_reduce(unsigned d,
                               const std::vector< math::vertex<double> >& src,
                               std::size_t n,
                               const math::matrix<double>& mv,
                               const std::vector< math::matrix<double> >& proj,
                               int mode,
                               std::size_t* degenerate) {
    std::vector<double> out(n * 4);

    std::size_t bad = 0;

    for(std::size_t j = 0; j < n; j++) {
        // One vertex constructed per source vertex, and the source read in
        // place -- which is what HPlot<T>::transform does.
        //
        // This used to build a second vertex and copy the input into it
        // every frame, which the app never does.  Removing it was a guess at
        // why the app's CPU path looked ~35% cheaper than this number, and
        // **the guess was wrong**: it moved D=14 from 20.0 ms to 20.1 and
        // D=18 from 510 ms to 494, which is noise.  Kept because the
        // reference should look like the thing it references, not because it
        // explained anything.
        //
        // The gap is on the other side.  The app's GPU path pays per-frame
        // costs this benchmark does not measure -- flattening the projection
        // matrices, and unpacking every result into a freshly allocated
        // math::vertex -- so the in-app GPU step costs more than the 1.9 ms
        // here and the subtraction that implied a cheap CPU was measuring
        // that overhead instead.  See #383.
        math::vertex<double> ret(d);

        ret = mv * src[j]();

        for(int s = int(d); s > 3; s--) {
            ret[s] = 1;
            ret = proj[d - s] * ret();

            const bool outermost = (s == int(d));

            if(mode == 2 || (mode == 1 && outermost)) {
                if(std::fabs(ret[s]) < 0.1)
                    bad++;

                ret.normalize();
            }

            ret.change(s - 1);
        }

        out[j * 4 + 0] = ret[0];
        out[j * 4 + 1] = ret[1];
        out[j * 4 + 2] = ret[2];
        out[j * 4 + 3] = ret[3];
    }

    if(degenerate)
        *degenerate = bad;

    return out;
}

}

int main(int argc, char** argv) {
    unsigned lo = 4;
    unsigned hi = 16;
    int mode = 2;                       // perspective, jhardhyper's default

    if(argc > 1) lo = unsigned(std::atoi(argv[1]));
    if(argc > 2) hi = unsigned(std::atoi(argv[2]));
    if(argc > 3) mode = std::atoi(argv[3]);

    std::printf("jhardhyper D->3 reduction, CPU double vs Metal float\n");
    std::printf("mode %d (0 ortho, 1 mixed, 2 perspective), vertices = 2^D\n\n", mode);
    std::printf("%3s %8s %11s %11s %8s %11s %9s %7s\n",
                "D", "verts", "cpu us/fr", "gpu us/fr", "speedup",
                "rel err", "extent", "near-w");
    std::printf("%3s %8s %11s %11s %8s %11s %9s %7s\n",
                "---", "--------", "-----------", "-----------",
                "--------", "-----------", "---------", "-------");

    for(unsigned d = lo; d <= hi; d++) {
        const std::size_t n = std::size_t(1) << d;
        const unsigned w = d + 1;

        const std::vector<double> verts = sphere(d, n, 20260928u);

        const std::vector< std::pair<double,double> > clip = clip_of(d);
        const math::matrix<double> mv = modelview_of(d);

        // Indexed the way jhardhyper indexes m_project: entry D-s is the
        // matrix for step s, for s from D down to 4.
        std::vector< math::matrix<double> > proj;
        std::vector< std::vector<double> > flat;

        for(unsigned s = d; s > 3; s--) {
            proj.push_back(math::matrix<double>::project(s, clip));
            flat.push_back(flatten(proj.back(), s + 1));
        }

        // Once, before timing: this is what a frame does not pay.
        std::shared_ptr<metal::hyper_reduce> gpu;

        try {
            gpu = metal::hyper_reduce::create(d);
            gpu->projections(flat);
            gpu->vertices(verts.data(), n);
        } catch(const std::exception& e) {
            std::printf("%3u  %s\n", d, e.what());
            continue;
        }

        const std::vector<double> mvflat = flatten(mv, w);

        // Enough repetitions that the clock is not what is being measured,
        // fewer as the work grows.  One warm pass each first, so neither is
        // charged for its first-touch page faults.
        const int reps = (n <= 4096) ? 200 : (n <= 65536 ? 20 : 5);

        // Built once, outside the timing: the app keeps its geometry in
        // math::vertex objects across frames and does not rebuild them.
        std::vector< math::vertex<double> > src;

        src.reserve(n);

        for(std::size_t j = 0; j < n; j++) {
            math::vertex<double> v(d);

            for(unsigned k = 0; k <= d; k++)
                v[k] = verts[j * w + k];

            src.push_back(std::move(v));
        }

        std::size_t degenerate = 0;

        std::vector<double> want =
            cpu_reduce(d, src, n, mv, proj, mode, &degenerate);

        gpu->run(mvflat.data(), metal::hyper_reduce::mode(mode));

        const clk::time_point a = clk::now();
        for(int r = 0; r < reps; r++)
            cpu_reduce(d, src, n, mv, proj, mode, 0);
        const clk::time_point b = clk::now();

        for(int r = 0; r < reps; r++)
            gpu->run(mvflat.data(), metal::hyper_reduce::mode(mode));
        const clk::time_point c = clk::now();

        const double cpu_us = micros(a, b) / reps;
        const double gpu_us = micros(b, c) / reps;

        const double diff =
            metal::hyper_reduce::divergence(gpu->result(), want.data(), n);

        // Relative to the figure, not absolute: see hyper_reduce::divergence.
        const double ext = metal::hyper_reduce::extent(want.data(), n);
        const double rel = (ext > 0) ? diff / ext : 0;

        std::printf("%3u %8zu %11.1f %11.1f %8.2fx %11.2e %9.2e %6.1f%%\n",
                    d, n, cpu_us, gpu_us, cpu_us / gpu_us, rel, ext,
                    100.0 * double(degenerate) / double(n * (d - 3)));

        std::fflush(stdout);
    }

    return 0;
}
