/* -*- mode: ObjC++ c-basic-offset: 4 -*-
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

#include <jlib/metal/hyper_reduce.hh>

#include <cmath>
#include <cstring>
#include <string>

namespace jlib {
namespace metal {

namespace {

/**
 * The kernel, with D substituted in as a literal before compilation.
 *
 * Transcribed from HPlot<T>::transform in jlib/apps/jhardhyper.cc.  Each step
 * there is three operations on a math::vertex -- reset the homogeneous
 * coordinate, multiply, conditionally divide -- and then change(d-1), which
 * drops the component just projected away and writes 1 into the slot below.
 * In a flat array "drop the top component and set the new last one to 1" is
 * just `x[d-1] = 1`, which is why there is no shrink here and no copy.
 */
const char* const SOURCE = R"MSL(
#include <metal_stdlib>
using namespace metal;

#define D @D@
#define W (D + 1)

struct params {
    uint n;
    uint mode;      // 0 orthographic, 1 mixed, 2 perspective
};

kernel void k_hyper_reduce(device const float* verts [[buffer(0)]],
                           device const float* mv    [[buffer(1)]],
                           device const float* proj  [[buffer(2)]],
                           device float*       out   [[buffer(3)]],
                           constant params&    p     [[buffer(4)]],
                           uint gid [[thread_position_in_grid]])
{
    // dispatchThreads shortens the tail group rather than padding it, so this
    // is belt and braces -- and it stays correct if that ever changes.
    if(gid >= p.n)
        return;

    // Thread-local, so both of these live in registers if they fit and spill
    // to device memory if they do not.  At W = 17 that is 136 bytes a thread
    // and the occupancy the pipeline reports drops accordingly, which is why
    // compute::call::run asks it rather than guessing a threadgroup size.
    float x[W];
    float y[W];

    for(uint i = 0; i < W; i++)
        x[i] = verts[gid * W + i];

    // ret = modelview.top() * vertex().  The modelview alone, not the mvp:
    // the projection stack is applied a step at a time below.
    for(uint i = 0; i < W; i++) {
        float s = 0;
        for(uint k = 0; k < W; k++)
            s += mv[i * W + k] * x[k];
        y[i] = s;
    }

    for(uint i = 0; i < W; i++)
        x[i] = y[i];

    for(uint d = D; d > 3; d--) {
        // Reset w.  Only the first pass through needs it -- every later one
        // finds the 1 that the previous step's change() wrote -- but it is
        // one store against a branch, and it keeps this identical to the CPU.
        x[d] = 1.0f;

        // Padded to a W stride, so every step indexes the same way even
        // though the matrix for step d is only (d+1) square.
        device const float* P = proj + (D - d) * W * W;

        const uint w = d + 1;

        for(uint i = 0; i < w; i++) {
            float s = 0;
            for(uint k = 0; k < w; k++)
                s += P[i * W + k] * x[k];
            y[i] = s;
        }

        for(uint i = 0; i < w; i++)
            x[i] = y[i];

        // Which steps divide is the projection mode: every one for
        // perspective, none for orthographic, only the outermost for mixed.
        if(p.mode == 2 || (p.mode == 1 && d == D)) {
            const float wc = x[d];
            for(uint i = 0; i < w; i++)
                x[i] /= wc;
        }

        // change(d-1): the component at d-1 was the depth this step just
        // projected away, and the slot becomes the new homogeneous one.
        x[d - 1] = 1.0f;
    }

    out[gid * 4 + 0] = x[0];
    out[gid * 4 + 1] = x[1];
    out[gid * 4 + 2] = x[2];
    out[gid * 4 + 3] = x[3];
}
)MSL";

std::string specialise(unsigned d) {
    std::string s = SOURCE;

    const std::string tag = "@D@";
    const std::string::size_type at = s.find(tag);

    if(at != std::string::npos)
        s.replace(at, tag.size(), std::to_string(d));

    return s;
}

struct uniforms {
    unsigned n;
    unsigned mode;
};

}

struct hyper_reduce::impl {
    unsigned d = 0;
    unsigned w = 0;         // d + 1, the padded stride
    std::size_t n = 0;
    bool ready = false;      // projections() has been called

    std::shared_ptr<compute> prog;

    std::shared_ptr<compute::buffer> verts;
    std::shared_ptr<compute::buffer> mv;
    std::shared_ptr<compute::buffer> proj;
    std::shared_ptr<compute::buffer> out;
};

hyper_reduce::hyper_reduce() : m_impl(new impl) {}
hyper_reduce::~hyper_reduce() {}

std::shared_ptr<hyper_reduce> hyper_reduce::create(unsigned d) {
    if(d < 4)
        throw compute::exception("a reduction to three needs at least four dimensions");

    std::shared_ptr<hyper_reduce> ret(new hyper_reduce);

    ret->m_impl->d = d;
    ret->m_impl->w = d + 1;
    ret->m_impl->prog = compute::build(specialise(d));

    const unsigned w = ret->m_impl->w;

    ret->m_impl->mv = ret->m_impl->prog->alloc(std::size_t(w) * w * sizeof(float));
    ret->m_impl->proj = ret->m_impl->prog->alloc(std::size_t(d - 3) * w * w * sizeof(float));

    return ret;
}

void hyper_reduce::projections(const std::vector< std::vector<double> >& proj) {
    const unsigned d = m_impl->d;
    const unsigned w = m_impl->w;

    // D-3 steps: d from D down to 4 inclusive.  Checked rather than trusted,
    // because the failure is a kernel reading past the end of the buffer,
    // which is not a crash -- it is wrong vertices.
    const std::size_t want = d - 3;

    if(proj.size() != want)
        throw compute::exception("expected " + std::to_string(want)
                                 + " projection matrices for D = " + std::to_string(d)
                                 + ", got " + std::to_string(proj.size()));

    float* p = static_cast<float*>(m_impl->proj->data());

    for(std::size_t s = 0; s < want; s++) {
        // Step D-s is (D-s+1) square; it goes into the top-left of a W-square
        // tile so the kernel can stride uniformly.
        const std::size_t side = d - s + 1;

        if(proj[s].size() != side * side)
            throw compute::exception("projection matrix " + std::to_string(s)
                                     + " should be " + std::to_string(side)
                                     + " square");

        for(std::size_t i = 0; i < side; i++)
            for(std::size_t j = 0; j < side; j++)
                p[s * w * w + i * w + j] = float(proj[s][i * side + j]);
    }

    m_impl->ready = true;
}

void hyper_reduce::vertices(const double* verts, std::size_t n) {
    const unsigned w = m_impl->w;

    m_impl->n = n;
    m_impl->verts = m_impl->prog->alloc(n * w * sizeof(float));
    m_impl->out = m_impl->prog->alloc(n * 4 * sizeof(float));

    float* v = static_cast<float*>(m_impl->verts->data());

    for(std::size_t i = 0; i < n * w; i++)
        v[i] = float(verts[i]);
}

std::size_t hyper_reduce::size() const {
    return m_impl->n;
}

void hyper_reduce::run(const double* modelview, mode m, float* out) {
    if(!m_impl->verts)
        throw compute::exception("no vertices have been given");

    if(!m_impl->ready)
        throw compute::exception("no projection matrices have been given");

    const unsigned w = m_impl->w;

    float* mv = static_cast<float*>(m_impl->mv->data());

    for(std::size_t i = 0; i < std::size_t(w) * w; i++)
        mv[i] = float(modelview[i]);

    uniforms u;

    u.n = unsigned(m_impl->n);
    u.mode = unsigned(m);

    m_impl->prog->kernel("k_hyper_reduce")
        .bind(m_impl->verts)
        .bind(m_impl->mv)
        .bind(m_impl->proj)
        .bind(m_impl->out)
        .bind(&u, sizeof(u))
        .run(m_impl->n);

    if(out)
        std::memcpy(out, m_impl->out->data(), m_impl->n * 4 * sizeof(float));
}

const float* hyper_reduce::result() const {
    return m_impl->out ? static_cast<const float*>(m_impl->out->data()) : 0;
}

double hyper_reduce::extent(const double* b, std::size_t n, unsigned per) {
    double worst = 0;

    for(std::size_t i = 0; i < n; i++) {
        for(unsigned k = 0; k < per; k++) {
            const double m = std::fabs(b[i * 4 + k]);

            if(m > worst)
                worst = m;
        }
    }

    return worst;
}

double hyper_reduce::divergence(const float* a, const double* b,
                                std::size_t n, unsigned per) {
    double worst = 0;

    for(std::size_t i = 0; i < n; i++) {
        for(unsigned k = 0; k < per; k++) {
            const double e = std::fabs(double(a[i * 4 + k]) - b[i * 4 + k]);

            if(e > worst)
                worst = e;
        }
    }

    return worst;
}

}
}
