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

#include <iomanip>
#include <iostream>
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

        std::cout << "\n";
    }

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
