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

#ifndef JLIB_METAL_HYPER_REDUCE_HH
#define JLIB_METAL_HYPER_REDUCE_HH

#include <jlib/metal/compute.hh>

#include <cstddef>
#include <memory>
#include <vector>

namespace jlib {
namespace metal {

/**
 * The D-to-3 projection chain of jhardhyper's transform(), on the GPU.
 *
 * One thread per vertex, running the whole chain: the vertices are
 * independent, so this is the shape a GPU is for.  The CPU version is
 * `HPlot<T>::transform` in jlib/apps/jhardhyper.cc, and this must track it --
 * the loop below is transcribed from it deliberately rather than rewritten.
 *
 * ## Why not gemm
 *
 * The obvious move is to batch the vertices into a matrix and call
 * metal::gemm.  It does not work, and the reason is worth stating so nobody
 * tries it again: in perspective mode **every step divides**, so the chain is
 * not linear and does not compose into one matrix.  A gemm per step with an
 * elementwise divide between would be D-3 dispatches a frame on work this
 * small, which is launch overhead with a computation attached.  One fused
 * kernel is the only shape that makes sense.
 *
 * (In `orthographic` the chain *is* linear and the whole thing collapses to a
 * single matrix on the CPU -- no GPU needed.  In `mixed` all but the outermost
 * step is linear, so it collapses to one matrix and one divide.  Neither is
 * jhardhyper's default, which is perspective; see jhardhyper.cc:294.)
 *
 * ## Float, and what that costs
 *
 * MSL has no `double`, and jhardhyper is `GLdouble`.  So this is not the same
 * computation -- it is the same computation in float, through as many as
 * thirteen chained divides, each of which can sit near a clipping plane where
 * the denominator is small.  **Do not assume the error is negligible**;
 * measure it.  divergence() exists for that and the benchmark reports it.
 *
 * ## Per-D specialisation
 *
 * The kernel is compiled with D as a literal, so the per-thread scratch is
 * exactly D+1 floats and the inner loops can unroll.  Changing D recompiles
 * (a millisecond or two), which is why D is fixed at construction rather than
 * passed per call: jhardhyper changes it on a keypress, not per frame.
 */
class hyper_reduce {
public:
    /** Which steps divide, matching math::Plot's projection_mode. */
    enum class mode { orthographic = 0, mixed = 1, perspective = 2 };

    /**
     * A reducer for dimension `d`.  Compiles the kernel, which is the
     * expensive part -- keep the result for as long as D does not change.
     */
    static std::shared_ptr<hyper_reduce> create(unsigned d);

    /**
     * The per-step projection matrices.  Cheap; safe to call per frame.
     *
     * Separate from create() because the two change on different clocks:
     * the kernel depends only on D, while these depend on the clip volume,
     * which moves whenever the window is resized.  math::Plot discards
     * m_project on either, so a caller that rebuilt the whole reducer to
     * follow it would recompile the kernel on every resize.
     *
     * Indexed the way jhardhyper indexes m_project -- entry `D - s` is the
     * matrix for step `s`, for s from D down to 4 -- and each entry is the
     * (s+1)-square matrix in row-major order.  That is D-3 matrices; a
     * caller handing over a different number gets an exception rather than a
     * kernel reading off the end.
     */
    void projections(const std::vector< std::vector<double> >& proj);

    ~hyper_reduce();

    /**
     * The geometry, uploaded once and kept.
     *
     * `verts` is `n` vertices of D+1 doubles each, row-major, homogeneous
     * coordinate last -- the layout of math::vertex's backing column.
     * Narrowed to float here, which is the only place the source geometry
     * loses precision; everything after is float throughout.
     */
    void vertices(const double* verts, std::size_t n);

    /**
     * Reduce every vertex with this modelview, into `out`.
     *
     * `modelview` is the (D+1)-square matrix row-major.  `out` receives
     * `n * 4` floats: three spatial and the homogeneous coordinate, which is
     * what the chain leaves at dimension 3.
     *
     * Blocks until the GPU is done, so `out` is readable on return.
     *
     * `out` may be null, in which case the result stays in the GPU buffer and
     * result() reads it in place.  That is not a micro-optimisation: at 32768
     * vertices the copy is half a megabyte, which is the same order as the
     * dispatch it would be measured alongside, so a benchmark that always
     * copies cannot tell the two apart.
     */
    void run(const double* modelview, mode m, float* out = 0);

    /**
     * The last run's output, in place: `n * 4` floats, or null before a run.
     *
     * Unified memory, so this is the buffer the GPU wrote and not a copy.
     * Valid until the next run() overwrites it.
     */
    const float* result() const;

    /** How many vertices vertices() was given. */
    std::size_t size() const;

    /**
     * The largest componentwise difference between two runs of `n` values.
     *
     * For comparing this against the CPU's doubles.  **Absolute, and on its
     * own that is not the number you want.**  The reduced figure is not unit
     * radius -- every perspective divide shrinks it by about one frustum
     * depth, so by D = 16 the coordinates are some 3^-13 of where they
     * started and an absolute error shrinks with them.  Taking this alone
     * shows divergence apparently *improving* with D, which is the figure
     * getting smaller and nothing else.
     *
     * Divide by extent() to get the fraction of the figure, which is what
     * decides whether a pixel moved -- jhardhyper rescales to the measured
     * radius before drawing, so relative error is what survives to the
     * screen.
     *
     * Both take a count of **vertices**, not of floats, and look only at
     * the first `per` components of each four.  That is not a convenience:
     * the fourth is the homogeneous coordinate, which change() sets to
     * exactly 1 on every path, so including it pins the extent at 1.0 for
     * every D and silently turns the ratio back into the absolute number it
     * was meant to replace.
     */
    static double divergence(const float* a, const double* b,
                             std::size_t n, unsigned per = 3);

    /** The largest absolute spatial component: the figure's extent. */
    static double extent(const double* b, std::size_t n, unsigned per = 3);

private:
    hyper_reduce();

    struct impl;
    std::unique_ptr<impl> m_impl;
};

}
}

#endif // JLIB_METAL_HYPER_REDUCE_HH
