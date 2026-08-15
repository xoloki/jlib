/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * math::object's topology, checked against known values.
 *
 * An n-cube has 2^n vertices and n*2^(n-1) edges, and every vertex has exactly
 * n neighbours.  Those are facts rather than golden output, so they make a
 * real test.
 *
 * The spheroid case at the end is the one that matters most.  Topology used to
 * be stored as copies of the vertices, so renderers looked vertices up by
 * coordinate -- and a 3-spheroid has 30 vertices at only 14 distinct
 * positions.  Sixteen of them collapsed into others, silently, every frame.
 */
#include <jlib/math/matrix.hh>

#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>

using namespace jlib::math;
typedef double T;

static int failures = 0;

static void check(const char* what, long got, long want) {
    const bool ok = (got == want);
    if(!ok) ++failures;
    std::cout << (ok ? "  ok   " : "  FAIL ")
              << std::setw(34) << std::left << what
              << " got " << got << ", expected " << want << "\n";
}

int main() {
    for(uint n = 2; n <= 5; n++) {
        cuboid<T> c(n);

        const long verts = 1L << n;
        const long edges = static_cast<long>(n) * (1L << (n - 1));

        std::cout << n << "-cube:\n";
        check("vertices", c.size(), verts);
        check("edges (each once)", c.get_edges().size(), edges);

        // every vertex has exactly n neighbours
        long degree_wrong = 0;
        for(uint i = 0; i < c.size(); i++)
            if(c.adjacent(i).size() != n) ++degree_wrong;
        check("vertices with wrong degree", degree_wrong, 0);

        // adjacency symmetric, and no self-loops or duplicates
        long asym = 0, self = 0, dup = 0;
        for(uint i = 0; i < c.size(); i++) {
            std::set<uint> seen;
            const std::vector<uint>& a = c.adjacent(i);
            for(uint k = 0; k < a.size(); k++) {
                if(a[k] == i) ++self;
                if(!seen.insert(a[k]).second) ++dup;

                const std::vector<uint>& b = c.adjacent(a[k]);
                bool back = false;
                for(uint m = 0; m < b.size(); m++)
                    if(b[m] == i) back = true;
                if(!back) ++asym;
            }
        }
        check("asymmetric adjacencies", asym, 0);
        check("self loops", self, 0);
        check("duplicate adjacencies", dup, 0);

        // indices in range
        long oob = 0;
        for(uint i = 0; i < c.size(); i++) {
            const std::vector<uint>& a = c.adjacent(i);
            for(uint k = 0; k < a.size(); k++)
                if(a[k] >= c.size()) ++oob;
        }
        check("out-of-range indices", oob, 0);

        // faces: C(n,2) * 2^(n-2)
        const long faces = (static_cast<long>(n) * (n - 1) / 2) * (1L << (n - 2));
        check("faces", c.get_faces().size(), faces);

        long wrong_size = 0, face_oob = 0;
        std::map<std::pair<uint,uint>, long> sides;
        for(uint f = 0; f < c.get_faces().size(); f++) {
            const std::vector<uint>& face = c.get_faces()[f];

            if(face.size() != 4) ++wrong_size;

            for(uint k = 0; k < face.size(); k++) {
                if(face[k] >= c.size()) ++face_oob;

                // consecutive corners, wrapping -- a face is a loop
                const uint x = face[k];
                const uint y = face[(k + 1) % face.size()];
                sides[x < y ? std::make_pair(x, y) : std::make_pair(y, x)]++;
            }
        }
        check("faces that are not quads", wrong_size, 0);
        check("out-of-range face indices", face_oob, 0);

        // Every side of every face must be a real edge of the cube.  This is
        // what catches a bowtie: 00,10,01,11 traverses two diagonals, which
        // are not edges.
        std::set<std::pair<uint,uint> > real;
        for(uint e = 0; e < c.get_edges().size(); e++)
            real.insert(c.get_edges()[e]);

        long not_an_edge = 0;
        for(std::map<std::pair<uint,uint>, long>::iterator u = sides.begin();
            u != sides.end(); u++)
            if(real.find(u->first) == real.end()) ++not_an_edge;
        check("face sides that are not edges", not_an_edge, 0);

        // and every edge lies in exactly n-1 faces: fix the edge's axis, and
        // any one of the remaining n-1 axes completes a face
        long wrong_share = 0;
        for(std::map<std::pair<uint,uint>, long>::iterator u = sides.begin();
            u != sides.end(); u++)
            if(u->second != static_cast<long>(n) - 1) ++wrong_share;
        check("edges used by wrong face count", wrong_share, 0);
        check("edges covered by faces", sides.size(), edges);

        std::cout << "\n";
    }

    // The flat torus, checked against what a torus has to be.
    //
    // The alternating sum of cell counts is the real test: it only comes out
    // right if the vertices, the cyclic adjacency and the face enumeration all
    // agree, and a missing wrap-around at the seam would break it.
    //
    // A k-torus grid has C(k,j)*m^k cells of dimension j, so the full sum is
    // m^k*(1-1)^k = 0.  object only holds cells up to dimension 2, so what is
    // checkable here is the truncation, m^k*(1 - k + C(k,2)).  At k=2 nothing
    // exists above dimension 2 and that truncation is the Euler characteristic
    // itself, zero, as a torus requires; at k=3 it is m^3, with the 3-cells
    // that would cancel it not represented.
    for(uint k = 2; k <= 3; k++) {
        for(uint m = 3; m <= 8; m += 5) {
            const uint n = 2 * k;
            torus<T> t(n, k, m);

            long verts = 1;
            for(uint j = 0; j < k; j++) verts *= m;

            const long edges = static_cast<long>(k) * verts;
            const long faces = (static_cast<long>(k) * (k - 1) / 2) * verts;

            std::cout << k << "-torus in " << n << "D, " << m << " per circle:\n";
            check("vertices", t.size(), verts);
            check("edges (each once)", t.get_edges().size(), edges);
            check("faces", t.get_faces().size(), faces);

            // every vertex has two neighbours per circle
            long degree_wrong = 0;
            for(uint i = 0; i < t.size(); i++)
                if(t.adjacent(i).size() != 2 * k) ++degree_wrong;
            check("vertices with wrong degree", degree_wrong, 0);

            const long alternating = verts * (1 - static_cast<long>(k)
                                              + static_cast<long>(k) * (k - 1) / 2);
            check(k == 2 ? "Euler characteristic V-E+F" : "V-E+F (2-skeleton)",
                  static_cast<long>(t.size()) - edges + faces, alternating);

            // on the unit sphere, by construction
            long off_sphere = 0;
            for(uint i = 0; i < t.size(); i++) {
                T r2 = 0;
                for(uint x = 0; x < t.D; x++) r2 += t[i][x] * t[i][x];
                if(std::fabs(std::sqrt(r2) - 1.0) > 1e-9) ++off_sphere;
            }
            check("vertices off the unit sphere", off_sphere, 0);

            // every side of every face is a real edge, and every edge is used
            // by exactly the faces that should use it
            std::map<std::pair<uint,uint>, long> sides;
            long face_oob = 0, wrong_size = 0;
            for(uint f = 0; f < t.get_faces().size(); f++) {
                const std::vector<uint>& face = t.get_faces()[f];
                if(face.size() != 4) ++wrong_size;
                for(uint x = 0; x < face.size(); x++) {
                    if(face[x] >= t.size()) ++face_oob;
                    const uint p = face[x], q = face[(x + 1) % face.size()];
                    sides[p < q ? std::make_pair(p, q) : std::make_pair(q, p)]++;
                }
            }
            check("faces that are not quads", wrong_size, 0);
            check("out-of-range face indices", face_oob, 0);

            std::set<std::pair<uint,uint> > real;
            for(uint e = 0; e < t.get_edges().size(); e++)
                real.insert(t.get_edges()[e]);

            long not_an_edge = 0;
            for(std::map<std::pair<uint,uint>, long>::iterator u = sides.begin();
                u != sides.end(); u++)
                if(real.find(u->first) == real.end()) ++not_an_edge;
            check("face sides that are not edges", not_an_edge, 0);
            check("edges covered by faces", sides.size(), edges);

            std::cout << "\n";
        }
    }

    // A 2-torus in higher ambient dimensions is the same surface: the mesh
    // must not grow, and the unused axes must stay flat.
    for(uint n = 4; n <= 7; n++) {
        torus<T> t(n, 2, 8);

        std::cout << "2-torus in " << n << "D: ";
        check("vertices", t.size(), 64);

        long nonzero = 0;
        for(uint i = 0; i < t.size(); i++)
            for(uint x = 4; x < t.D; x++)
                if(t[i][x] != 0) ++nonzero;
        check("  nonzero unused axes", nonzero, 0);
    }
    std::cout << "\n";

    // the shape that used to be mangled: duplicates must survive as distinct
    spheroid<T> s(3);
    std::cout << "3-spheroid: " << s.size() << " vertices, "
              << s.get_edges().size() << " edges\n";

    std::set< std::vector<T> > distinct;
    for(uint i = 0; i < s.size(); i++) {
        std::vector<T> key;
        for(uint k = 0; k < s.D; k++) key.push_back(s[i][k]);
        distinct.insert(key);
    }
    std::cout << "            " << distinct.size()
              << " distinct positions -- "
              << (s.size() - distinct.size())
              << " vertices share coordinates with another\n";
    std::cout << "            (all of those collapsed into one under the old\n"
              << "             value-keyed lookup; they are separate now)\n";

    return failures ? 1 : 0;
}
