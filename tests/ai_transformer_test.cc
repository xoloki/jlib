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
 *
 */

#include <jlib/ai/transformer.hh>

#ifdef HAVE_METAL
#include <jlib/metal/backend.hh>
#endif

#include <cmath>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace ai = jlib::ai;

using jlib::math::matrix;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

template<typename T>
static matrix<T> random_matrix(uint m, uint n, std::mt19937& gen, float scale = 1.0f) {
    std::uniform_real_distribution<float> d(-scale, scale);

    matrix<T> a(m, n);

    for(uint r = 0; r < m; r++)
        for(uint c = 0; c < n; c++)
            a(r, c) = T(d(gen));

    return a;
}

template<typename T>
static double worst(const matrix<T>& a, const matrix<T>& b) {
    double w = 0;

    for(uint r = 0; r < a.M; r++)
        for(uint c = 0; c < a.N; c++)
            w = std::max(w, std::fabs(double(float(a(r,c))) - double(float(b(r,c)))));

    return w;
}

static const unsigned int D = 8;
static const unsigned int H = 2;
static const unsigned int F = 16;
static const unsigned int N = 5;

/** Every weight in a block, so two backends can be given the same one. */
template<typename T>
struct weights {
    std::vector<matrix<T> > wq, wk, wv, wo;
    matrix<T> w1, w3, w2, an, fn;

    weights(std::mt19937& gen)
        : w1(F, D), w3(F, D), w2(D, F), an(D, 1), fn(D, 1)
    {
        for(unsigned int h = 0; h < H; h++) {
            wq.push_back(random_matrix<T>(D / H, D, gen, 0.5f));
            wk.push_back(random_matrix<T>(D / H, D, gen, 0.5f));
            wv.push_back(random_matrix<T>(D / H, D, gen, 0.5f));
            wo.push_back(random_matrix<T>(D, D / H, gen, 0.5f));
        }

        w1 = random_matrix<T>(F, D, gen, 0.5f);
        w3 = random_matrix<T>(F, D, gen, 0.5f);
        w2 = random_matrix<T>(D, F, gen, 0.5f);

        for(unsigned int r = 0; r < D; r++) { an(r,0) = T(1.0f); fn(r,0) = T(1.0f); }
    }
};

template<typename T>
static void load(ai::block<T>& blk, const weights<T>& w) {
    for(unsigned int h = 0; h < H; h++) {
        blk.wq(h)->write(w.wq[h]);
        blk.wk(h)->write(w.wk[h]);
        blk.wv(h)->write(w.wv[h]);
        blk.wo(h)->write(w.wo[h]);
    }

    blk.w_gate()->write(w.w1);
    blk.w_up()->write(w.w3);
    blk.w_down()->write(w.w2);
    blk.attn_norm()->write(w.an);
    blk.ffn_norm()->write(w.fn);
}

template<typename T>
static matrix<T> run(ai::backend<T>& b, const weights<T>& w, const matrix<T>& x,
                     bool causal = true, bool rope = false)
{
    ai::block<T> blk(b, D, H, F);

    load(blk, w);

    if(rope) blk.set_rope(true);

    blk.reserve(N);

    typename ai::backend<T>::tensor_ptr tx = b.make(x);
    typename ai::backend<T>::tensor_ptr out = b.make(D, N);

    blk.forward(tx, out, causal);
    b.wait();

    return out->read();
}

/**
 * With both output projections zeroed, the block is the identity.
 *
 * Exactly the identity, not nearly: out = x + Wo(...) + W2(...), and both
 * terms are a multiply by a zero matrix.  Everything else still runs -- the
 * norms, the attention, the whole feed-forward -- so this says the residual
 * path is a clean sum and nothing leaks into it.
 */
template<typename T>
static void a_zeroed_block_is_the_identity(const char* name,
                                           std::vector<ai::backend<T>*>& backends)
{
    std::cout << "\na zeroed block is the identity, " << name << ":\n";

    std::mt19937 gen(3);

    weights<T> w(gen);

    for(unsigned int h = 0; h < H; h++)
        for(unsigned int r = 0; r < D; r++)
            for(unsigned int c = 0; c < D / H; c++)
                w.wo[h](r,c) = T(0.0f);

    for(unsigned int r = 0; r < D; r++)
        for(unsigned int c = 0; c < F; c++)
            w.w2(r,c) = T(0.0f);

    const matrix<T> x = random_matrix<T>(D, N, gen);

    for(ai::backend<T>* b : backends) {
        const matrix<T> got = run(*b, w, x);

        ok(std::string("  ") + b->name() + ": out is bit-identical to x",
           worst(got, x) == 0.0, std::to_string(worst(got, x)));
    }
}

/**
 * The whole block looks only backwards.
 *
 * Attention is masked, and everything else -- both norms and the entire
 * feed-forward -- is position-wise, so a change at the last position must not
 * reach any earlier output at all.  Bit-identical again, for the same reason
 * it was in the attention test: a masked score contributes exactly zero.
 *
 * This is the assertion that covers the assembly rather than the parts.
 */
template<typename T>
static void the_block_looks_only_backwards(const char* name,
                                           std::vector<ai::backend<T>*>& backends)
{
    std::cout << "\nthe block looks only backwards, " << name << ":\n";

    std::mt19937 gen(11);

    const weights<T> w(gen);

    const matrix<T> x = random_matrix<T>(D, N, gen);

    matrix<T> x2 = x;

    for(unsigned int r = 0; r < D; r++)
        x2(r, N - 1) = T(float(x2(r, N - 1)) + 2.0f);

    for(ai::backend<T>* b : backends) {
        const matrix<T> base = run(*b, w, x,  true);
        const matrix<T> pert = run(*b, w, x2, true);

        bool same = true;

        for(unsigned int c = 0; c + 1 < N; c++)
            for(unsigned int r = 0; r < D; r++)
                if(float(base(r,c)) != float(pert(r,c))) same = false;

        ok(std::string("  ") + b->name() +
           ": a change at the last position leaves every earlier one alone", same);

        // The control, as in the attention test: without the mask it must
        // reach backwards, or the assertion above is passing on its own.
        const matrix<T> ob = run(*b, w, x,  false);
        const matrix<T> op = run(*b, w, x2, false);

        bool moved = false;

        for(unsigned int c = 0; c + 1 < N; c++)
            for(unsigned int r = 0; r < D; r++)
                if(float(ob(r,c)) != float(op(r,c))) moved = true;

        ok(std::string("  ") + b->name() + ": and uncausal it does not", moved);
    }
}

/**
 * Heads are separate, and each is routed through its own output projection.
 *
 * With head 1's output projection zeroed, head 1's query projection cannot
 * reach the output -- and head 0's still must, or the test would pass on a
 * block that ignored its input entirely.
 */
template<typename T>
static void heads_are_routed_separately(const char* name,
                                        std::vector<ai::backend<T>*>& backends)
{
    std::cout << "\nheads are routed separately, " << name << ":\n";

    std::mt19937 gen(23);

    weights<T> w(gen);

    for(unsigned int r = 0; r < D; r++)
        for(unsigned int c = 0; c < D / H; c++)
            w.wo[1](r,c) = T(0.0f);

    const matrix<T> x = random_matrix<T>(D, N, gen);

    weights<T> bumped_1 = w;
    weights<T> bumped_0 = w;

    for(unsigned int r = 0; r < D / H; r++)
        for(unsigned int c = 0; c < D; c++) {
            bumped_1.wq[1](r,c) = T(float(bumped_1.wq[1](r,c)) + 1.0f);
            bumped_0.wq[0](r,c) = T(float(bumped_0.wq[0](r,c)) + 1.0f);
        }

    for(ai::backend<T>* b : backends) {
        const matrix<T> base = run(*b, w, x);

        ok(std::string("  ") + b->name() +
           ": a silenced head's query projection cannot reach the output",
           worst(base, run(*b, bumped_1, x)) == 0.0);

        ok(std::string("  ") + b->name() + ": while a live head's still does",
           worst(base, run(*b, bumped_0, x)) > 0.0);
    }
}

/**
 * The heads are summed, not overwritten.
 *
 * Give both heads identical weights.  Then routing everything through head 0
 * and splitting it evenly between the two must give the same answer, since
 * Wo*o == (Wo/2)*o + (Wo/2)*o.  That is the GEMM's beta accumulation, which is
 * the one part of the head machinery a per-head test cannot see.
 */
template<typename T>
static void the_heads_are_summed(const char* name,
                                 std::vector<ai::backend<T>*>& backends)
{
    std::cout << "\nthe heads are summed, " << name << ":\n";

    std::mt19937 gen(29);

    weights<T> one(gen);

    one.wq[1] = one.wq[0];
    one.wk[1] = one.wk[0];
    one.wv[1] = one.wv[0];
    one.wo[1] = one.wo[0];

    weights<T> split = one;

    for(unsigned int r = 0; r < D; r++)
        for(unsigned int c = 0; c < D / H; c++) {
            const float half = float(one.wo[0](r,c)) * 0.5f;

            split.wo[0](r,c) = T(half);
            split.wo[1](r,c) = T(half);
        }

    // And the single-head reference: all of it through head 0, none through 1.
    weights<T> only_0 = one;

    for(unsigned int r = 0; r < D; r++)
        for(unsigned int c = 0; c < D / H; c++)
            only_0.wo[1](r,c) = T(0.0f);

    const matrix<T> x = random_matrix<T>(D, N, gen);

    for(ai::backend<T>* b : backends) {
        const double d = worst(run(*b, only_0, x), run(*b, split, x));

        ok(std::string("  ") + b->name() +
           ": one head at full weight equals two at half",
           d < ((sizeof(T) == 2) ? 5e-3 : 1e-6), std::to_string(d));
    }
}

/** silu, and the one thing it cannot do. */
template<typename T>
static void silu_is_x_times_sigmoid(const char* name,
                                    std::vector<ai::backend<T>*>& backends)
{
    std::cout << "\nsilu is x times sigmoid, " << name << ":\n";

    std::mt19937 gen(31);

    const matrix<T> x = random_matrix<T>(4, 3, gen, 4.0f);

    for(ai::backend<T>* b : backends) {
        typename ai::backend<T>::tensor_ptr tx = b->make(x);
        typename ai::backend<T>::tensor_ptr ty = b->make(4, 3);

        b->activate(ai::activation::silu, tx, ty);
        b->wait();

        const matrix<T> got = ty->read();

        double furthest = 0;

        for(unsigned int r = 0; r < 4; r++)
            for(unsigned int c = 0; c < 3; c++) {
                const double v = double(float(x(r,c)));
                const double want = v / (1.0 + std::exp(-v));

                furthest = std::max(furthest, std::fabs(want - double(float(got(r,c)))));
            }

        ok(std::string("  ") + b->name() + ": matches x / (1 + exp(-x))",
           furthest < ((sizeof(T) == 2) ? 5e-3 : 1e-6), std::to_string(furthest));

        // silu is not injective -- it dips to about -0.278 near x = -1.278 and
        // rises on both sides -- so its derivative is not a function of its
        // output, and slope() has nothing but the output.  Refused rather than
        // answered wrongly.
        bool threw = false;

        try { b->slope(ai::activation::silu, ty, tx); b->wait(); }
        catch(std::exception&) { threw = true; }

        ok(std::string("  ") + b->name() + ": and its slope is refused, not guessed",
           threw);
    }
}

/**
 * Normalising the *branch input* makes the branch scale-invariant.
 *
 * rms_norm divides a column by its own RMS, so rms_norm(c*x) is rms_norm(x) --
 * which means the attention branch sees the same thing whatever x is scaled
 * by, while the residual path scales with it.  So block(c*x) - c*x must equal
 * block(x) - x.
 *
 * This is what tells pre-norm from no norm at all.  Every other assertion here
 * survives replacing rms_norm with a plain copy -- verified by mutation --
 * because a block without normalisation is still causal, still additive, and
 * still agrees with itself on both backends.
 *
 * The feed-forward is switched off for it, since the second residual's input is
 * not a clean multiple of the first's and the argument does not carry through.
 * Not exact: eps sits inside the square root, so rms_norm(c*x) and rms_norm(x)
 * differ by about eps/2 * (1 - 1/c^2).
 */
template<typename T>
static void the_branch_input_is_normalised(const char* name,
                                           std::vector<ai::backend<T>*>& backends)
{
    std::cout << "\nthe branch input is normalised, " << name << ":\n";

    std::mt19937 gen(41);

    weights<T> w(gen);

    for(unsigned int r = 0; r < D; r++)
        for(unsigned int c = 0; c < F; c++)
            w.w2(r,c) = T(0.0f);

    const matrix<T> x = random_matrix<T>(D, N, gen);

    matrix<T> x2 = x;

    for(unsigned int r = 0; r < D; r++)
        for(unsigned int c = 0; c < N; c++)
            x2(r,c) = T(float(x(r,c)) * 2.0f);

    for(ai::backend<T>* b : backends) {
        const matrix<T> a = run(*b, w, x);
        const matrix<T> d = run(*b, w, x2);

        double furthest = 0;

        for(unsigned int r = 0; r < D; r++)
            for(unsigned int c = 0; c < N; c++) {
                const double one = double(float(a(r,c))) - double(float(x(r,c)));
                const double two = double(float(d(r,c))) - double(float(x2(r,c)));

                furthest = std::max(furthest, std::fabs(one - two));
            }

        ok(std::string("  ") + b->name() +
           ": doubling the input leaves the attention branch where it was",
           furthest < ((sizeof(T) == 2) ? 2e-2 : 1e-4), std::to_string(furthest));
    }
}

/**
 * Position, which the block did not have until now.
 *
 * Without a positional encoding and without the mask, a block is
 * permutation-equivariant: it treats its input columns as a set, so reordering
 * them just reorders the outputs.  That is not a defect to be worked around --
 * it is what attention *is*, and it is exactly why a position has to be mixed
 * in somewhere.
 *
 * So this asserts the property both ways round.  Without rope the block
 * commutes with a permutation; with rope it does not, which is the whole
 * reason rope exists.  The uncausal run is the one to test on, since the mask
 * already breaks the symmetry by itself.
 */
template<typename T>
static void rope_gives_the_block_a_sense_of_position(
    const char* name, std::vector<ai::backend<T>*>& backends)
{
    std::cout << "\nrope gives the block a sense of position, " << name << ":\n";

    std::mt19937 gen(43);

    const weights<T> w(gen);

    const matrix<T> x = random_matrix<T>(D, N, gen);

    // One swap is enough to be a permutation, and keeping it simple keeps the
    // inverse obvious.
    const unsigned int i = 1;
    const unsigned int j = 3;

    matrix<T> swapped = x;

    for(unsigned int r = 0; r < D; r++) {
        swapped(r,i) = x(r,j);
        swapped(r,j) = x(r,i);
    }

    for(ai::backend<T>* b : backends) {
        const matrix<T> plain = run(*b, w, x,       false, false);
        const matrix<T> perm  = run(*b, w, swapped, false, false);

        // block(swap(x)) should be swap(block(x)).
        double furthest = 0;

        for(unsigned int c = 0; c < N; c++) {
            const unsigned int from = (c == i) ? j : (c == j) ? i : c;

            for(unsigned int r = 0; r < D; r++)
                furthest = std::max(furthest,
                                    std::fabs(double(float(perm(r,c))) -
                                              double(float(plain(r,from)))));
        }

        ok(std::string("  ") + b->name() +
           ": without rope the block treats its input as a set",
           furthest < ((sizeof(T) == 2) ? 5e-3 : 1e-5), std::to_string(furthest));

        const matrix<T> rplain = run(*b, w, x,       false, true);
        const matrix<T> rperm  = run(*b, w, swapped, false, true);

        double moved = 0;

        for(unsigned int c = 0; c < N; c++) {
            const unsigned int from = (c == i) ? j : (c == j) ? i : c;

            for(unsigned int r = 0; r < D; r++)
                moved = std::max(moved,
                                 std::fabs(double(float(rperm(r,c))) -
                                           double(float(rplain(r,from)))));
        }

        ok(std::string("  ") + b->name() + ": and with rope it does not",
           moved > 1e-3, std::to_string(moved));
    }
}

template<typename T>
static void the_backends_agree(const char* name,
                               std::vector<ai::backend<T>*>& backends)
{
    std::cout << "\nthe backends agree, " << name << ":\n";

    std::mt19937 gen(37);

    const weights<T> w(gen);
    const matrix<T> x = random_matrix<T>(D, N, gen);

    std::vector<matrix<T> > got;

    for(ai::backend<T>* b : backends) got.push_back(run(*b, w, x));

    for(std::size_t i = 1; i < backends.size(); i++)
        ok(std::string("  ") + backends[i]->name() + " agrees with host",
           worst(got[0], got[i]) < ((sizeof(T) == 2) ? 5e-2 : 1e-4),
           std::to_string(worst(got[0], got[i])));
}

template<typename T>
static void the_shapes_are_checked(const char* name, ai::backend<T>& b) {
    std::cout << "\nthe shapes are checked, " << name << ":\n";

    bool threw = false;
    try { ai::block<T> bad(b, 8, 3, 16); }
    catch(std::exception&) { threw = true; }

    ok("  heads must divide d_model", threw);

    ai::block<T> blk(b, D, H, F);

    typename ai::backend<T>::tensor_ptr x = b.make(D, N);
    typename ai::backend<T>::tensor_ptr out = b.make(D, N);

    threw = false;
    try { blk.forward(x, out); }
    catch(std::exception&) { threw = true; }

    ok("  forward before reserve is refused", threw);

    blk.reserve(N);

    typename ai::backend<T>::tensor_ptr wrong = b.make(D, N + 1);

    threw = false;
    try { blk.forward(wrong, out); }
    catch(std::exception&) { threw = true; }

    ok("  and a sequence of the wrong length is too", threw);
}

template<typename T>
static void everything(const char* name, std::vector<ai::backend<T>*>& b) {
    a_zeroed_block_is_the_identity<T>(name, b);
    the_block_looks_only_backwards<T>(name, b);
    heads_are_routed_separately<T>(name, b);
    the_heads_are_summed<T>(name, b);
    the_branch_input_is_normalised<T>(name, b);
    rope_gives_the_block_a_sense_of_position<T>(name, b);
    silu_is_x_times_sigmoid<T>(name, b);
    the_backends_agree<T>(name, b);
    the_shapes_are_checked<T>(name, *b[0]);
}

int main() {
    std::cout << std::unitbuf;

    {
        ai::host_backend<float> h;
        std::vector<ai::backend<float>*> b{ &h };

#ifdef HAVE_METAL
        std::shared_ptr<jlib::metal::backend<float> > g;
        try { g.reset(new jlib::metal::backend<float>); b.push_back(g.get()); }
        catch(std::exception& e) {
            // A failure, not a note.  HAVE_METAL means Metal was found when
            // this was configured, so the backend not coming up is a bug here
            // -- most likely a kernel that no longer compiles.  Reported as a
            // note, this test went on to pass with only the host backend and
            // exit 0, which is how a broken kernel reaches a green run.
            ok("  the Metal backend comes up", false, e.what());
        }
#endif
        everything<float>("float", b);
    }

    {
        ai::host_backend<_Float16> h;
        std::vector<ai::backend<_Float16>*> b{ &h };

#ifdef HAVE_METAL
        std::shared_ptr<jlib::metal::backend<_Float16> > g;
        try { g.reset(new jlib::metal::backend<_Float16>); b.push_back(g.get()); }
        catch(std::exception& e) { ok("  the Metal backend comes up", false, e.what()); }
#endif
        everything<_Float16>("_Float16", b);
    }

    // What a green run does not establish.
    //
    // That the block is a *correct* transformer block, only that it is a
    // self-consistent one.  Nothing here compares against a reference
    // implementation, because there is no loader yet and so no real weights to
    // compare with.
    //
    // Specifically and by measurement: swapping silu onto the up projection
    // instead of the gate fails nothing here.  Which of the two is the gate is
    // a convention that only a real model's weights can settle, which is why
    // they are called w_gate() and w_up() rather than w1() and w3() -- the
    // interface states it because the tests cannot check it.
    //
    // Which of the two rope layouts a model wants.  Both are rotations, both
    // satisfy every property checked above, and they are not compatible with
    // each other -- so what is exercised here is the wiring, never the choice.
    // See ai::rope_layout.
    //
    // Nor that rope belongs on the queries and keys and not on the values.
    // Measured: rotating the values as well fails nothing here.  The reason it
    // is wrong is that a value carries what a position said rather than which
    // position said it, so rotating it would make the content attended *to*
    // depend on where it sat -- an argument about meaning, which no property
    // test reaches.
    //
    // Not a stack.  One block is tested; nothing checks that N of them compose,
    // and pre-norm exists precisely for what happens at depth.
    //
    // Not training.  forward() only, and silu deliberately refuses to give a
    // slope, so this block cannot currently be backpropagated through.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
