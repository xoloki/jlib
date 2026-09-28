/* -*- mode: C++ c-basic-offset: 4 -*- */

// The GPU reduction, against the CPU chain it replaces.
//
// The oracle is math::matrix and math::vertex driven through the same steps
// HPlot<T>::transform takes -- naive, slow, and double, which is what makes
// it worth comparing against.  Not compared exactly: the kernel is float
// because MSL has no double, so the tolerance below is a float epsilon with
// room, relative to the extent of the figure rather than absolute.
//
// See #376 for why relative: every perspective divide shrinks the figure, so
// an absolute tolerance silently loosens as D grows.

#include <jlib/metal/hyper_reduce.hh>

#include <jlib/math/matrix.hh>

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace metal = jlib::metal;

using jlib::math::matrix;
using jlib::math::vertex;

static int failures = 0;

/** Scientific, because to_string prints anything under 1e-6 as 0.000000 --
 *  which is every passing run here, and would leave a future failure with no
 *  number to read. */
static std::string sci(double x) {
    char buf[32];

    std::snprintf(buf, sizeof(buf), "%.2e", x);

    return buf;
}

static void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << "\n";

    if(!ok)
        failures++;
}

/** jhardhyper's clip volume; see initialize_glazzies. */
static std::vector< std::pair<double,double> > clip_of(unsigned d) {
    const double r22 = 1.5;

    std::vector< std::pair<double,double> > clip;

    clip.push_back(std::make_pair(-r22, r22));
    clip.push_back(std::make_pair(-r22, r22));

    if(d > 2)
        clip.push_back(std::make_pair(-r22, r22));

    for(unsigned i = 3; i < d; i++)
        clip.push_back(std::make_pair(r22, 3 * r22));

    return clip;
}

static std::vector<double> flatten(const matrix<double>& m, unsigned side) {
    std::vector<double> ret(std::size_t(side) * side);

    for(unsigned i = 0; i < side; i++)
        for(unsigned j = 0; j < side; j++)
            ret[std::size_t(i) * side + j] = m(i, j);

    return ret;
}

/** A handful of points on the unit sphere, deterministically. */
static std::vector<double> points(unsigned d, std::size_t n) {
    const unsigned w = d + 1;

    std::vector<double> ret(n * w, 0.0);

    for(std::size_t i = 0; i < n; i++) {
        double r2 = 0;

        for(unsigned k = 0; k < d; k++) {
            // Deterministic and spread out, without leaning on a
            // distribution: those are implementation-defined and this test
            // runs on more than one standard library.
            const double x = std::sin(double(i + 1) * 0.7 + double(k) * 1.3);

            ret[i * w + k] = x;
            r2 += x * x;
        }

        const double r = (std::sqrt(r2) > 1e-12) ? std::sqrt(r2) : 1;

        for(unsigned k = 0; k < d; k++)
            ret[i * w + k] /= r;

        ret[i * w + d] = 1;
    }

    return ret;
}

/** The CPU chain, transcribed from HPlot<T>::transform. */
static std::vector<double> cpu(unsigned d,
                               const std::vector<double>& verts,
                               std::size_t n,
                               const matrix<double>& mv,
                               const std::vector< matrix<double> >& proj,
                               int mode) {
    const unsigned w = d + 1;

    std::vector<double> out(n * 4, 0.0);

    for(std::size_t j = 0; j < n; j++) {
        vertex<double> v(d);

        for(unsigned k = 0; k <= d; k++)
            v[k] = verts[j * w + k];

        vertex<double> ret(d);

        ret = mv * v();

        for(int s = int(d); s > 3; s--) {
            ret[s] = 1;
            ret = proj[d - s] * ret();

            if(mode == 2 || (mode == 1 && s == int(d)))
                ret.normalize();

            ret.change(s - 1);
        }

        for(unsigned k = 0; k < 4; k++)
            out[j * 4 + k] = ret[k];
    }

    return out;
}

static void agrees_with_the_cpu(unsigned d, int mode) {
    const std::size_t n = 64;
    const unsigned w = d + 1;

    const std::vector<double> verts = points(d, n);
    const std::vector< std::pair<double,double> > clip = clip_of(d);

    matrix<double> mv = matrix<double>::identity(w);

    // The projected-away axes pushed in front of their frustum; a positive
    // offset puts every vertex behind the eye.
    for(unsigned i = 3; i < d; i++)
        mv(i, d) = -3.0;

    std::vector< matrix<double> > proj;
    std::vector< std::vector<double> > flat;

    for(unsigned s = d; s > 3; s--) {
        proj.push_back(matrix<double>::project(s, clip));
        flat.push_back(flatten(proj.back(), s + 1));
    }

    std::shared_ptr<metal::hyper_reduce> gpu = metal::hyper_reduce::create(d);

    gpu->projections(flat);
    gpu->vertices(verts.data(), n);
    gpu->run(flatten(mv, w).data(), metal::hyper_reduce::mode(mode));

    const std::vector<double> want = cpu(d, verts, n, mv, proj, mode);

    const double ext = metal::hyper_reduce::extent(want.data(), n);
    const double err = metal::hyper_reduce::divergence(gpu->result(), want.data(), n);
    const double rel = (ext > 0) ? err / ext : err;

    check(ext > 0, "D=" + std::to_string(d) + " mode " + std::to_string(mode)
                 + ": the figure is not degenerate");

    check(rel < 1e-5, "D=" + std::to_string(d) + " mode " + std::to_string(mode)
                    + ": matches the CPU, relative " + sci(rel));
}

int main() {
    try {
        // Enough of a spread to catch a step-indexing error: D=4 takes one
        // step, D=9 takes six.
        agrees_with_the_cpu(4, 2);
        agrees_with_the_cpu(6, 2);
        agrees_with_the_cpu(9, 2);

        // The other two modes differ only in which steps divide, which is
        // exactly the sort of condition that gets inverted.
        //
        // Note that mode 0 is the weak one of the three: transposing the
        // projection matrices -- which the perspective cases above catch by
        // a factor of 10^7 -- leaves it unchanged to the last digit, because
        // without a divide the corrupted entries only ever reach components
        // that change() then discards.  It is here for the mode selection,
        // not as a check on the arithmetic.
        agrees_with_the_cpu(7, 0);
        agrees_with_the_cpu(7, 1);

        // The extent must ignore the homogeneous coordinate.  change() sets
        // it to 1 on every path, so counting it pins the extent at 1.0 for
        // every D and turns a relative error back into an absolute one --
        // which is a bug this measurement actually had, and which made
        // divergence appear to *improve* with D.
        {
            std::vector<double> v(8, 0.0);

            v[0] = 0.01; v[1] = 0.02; v[2] = 0.005; v[3] = 1.0;
            v[4] = 0.03; v[5] = 0.01; v[6] = 0.002; v[7] = 1.0;

            const double ext = metal::hyper_reduce::extent(v.data(), 2);

            check(std::fabs(ext - 0.03) < 1e-12,
                  "extent skips the homogeneous coordinate, got "
                  + sci(ext));
        }

        // A wrong count is a kernel reading past the end of a buffer, which
        // is wrong vertices rather than a crash -- so it must be refused.
        {
            bool threw = false;

            try {
                std::shared_ptr<metal::hyper_reduce> g = metal::hyper_reduce::create(8);

                g->projections(std::vector< std::vector<double> >(3));
            } catch(std::exception&) { threw = true; }

            check(threw, "too few projection matrices is refused");
        }

        {
            bool threw = false;

            try { metal::hyper_reduce::create(3); }
            catch(std::exception&) { threw = true; }

            check(threw, "a reduction from three dimensions is refused");
        }

        {
            bool threw = false;

            try {
                std::shared_ptr<metal::hyper_reduce> g = metal::hyper_reduce::create(5);
                std::vector<double> mv(36, 0.0);

                g->run(mv.data(), metal::hyper_reduce::mode::perspective);
            } catch(std::exception&) { threw = true; }

            check(threw, "running before any vertices are given is refused");
        }
    } catch(std::exception& e) {
        // 77 is SKIP.  A machine with no Metal device is not a failure.
        std::cout << "  skip  no Metal device: " << e.what() << "\n";
        return 77;
    }

    std::cout << (failures ? "FAILED" : "PASSED") << "\n";

    return failures ? 1 : 0;
}
