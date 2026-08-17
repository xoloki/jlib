// The lock-free ring buffer.
//
// Two halves.  The single-threaded part pins down the arithmetic -- fill,
// space, wrap-around, short reads and writes -- where a mistake is a wrong
// number and easy to see.  The threaded part runs a producer against a
// consumer and checks that every item arrives exactly once, in order, and
// whole.
//
// What that second part does not do is verify the memory ordering, and it is
// worth saying so rather than leaving the impression it does.  ThreadSanitizer
// does not help either: it finds races on ordinary accesses, but once the
// synchronization is atomic it takes the atomics at their word and will not
// tell you an ordering is too weak.  Checked -- replacing every acquire and
// release in ringbuffer.hh with relaxed leaves both this test and TSan
// perfectly happy, on hardware where it could genuinely tear.
//
// So the ordering rests on the argument in ringbuffer.hh, not on this test.
// Each item does carry fields derived from its value, so one published before
// its contents were written would show up as inconsistent rather than merely
// late -- but that did not catch the all-relaxed version either, over several
// runs.  The window is too narrow to hit: a chunk is a kilobyte of stores, and
// the counter is stored after them, so by the time a consumer looks the data
// has drained regardless of what the ordering permits.
//
// The point of saying this is that a green result here is evidence the
// arithmetic and the handoff work, and is not evidence the ordering is
// sufficient.  Confirming that needs a tool that models the memory model --
// relacy, CDSChecker, GenMC -- rather than a stress test.
#include <jlib/sys/ringbuffer.hh>

#include <iostream>
#include <thread>
#include <vector>

using jlib::sys::ringbuffer;

static int failures = 0;

static void check(const char* what, long got, long want) {
    const bool ok = (got == want);
    if(!ok) ++failures;
    std::cout << (ok ? "  ok   " : "  FAIL ") << what
              << ": got " << got << ", expected " << want << "\n";
}

static void single_threaded() {
    std::cout << "single threaded:\n";

    ringbuffer<int> r(8);

    check("capacity", r.capacity(), 8);
    check("starts empty", r.readable(), 0);
    check("starts writable", r.writable(), 8);

    int in[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    check("write 5", r.write(in, 5), 5);
    check("readable after 5", r.readable(), 5);
    check("writable after 5", r.writable(), 3);

    // the whole capacity is usable: no slot is given up to tell full from empty
    check("write 5 more, room for 3", r.write(in, 5), 3);
    check("full", r.writable(), 0);
    check("write when full", r.write(in, 1), 0);

    int out[16] = { 0 };
    check("read 4", r.read(out, 4), 4);
    check("first four values", out[0] + out[1]*10 + out[2]*100 + out[3]*1000, 0 + 10 + 200 + 3000);

    // and now the interesting case: writing past the end of the array
    check("write 4 across the wrap", r.write(in, 4), 4);
    check("readable", r.readable(), 8);

    // drain and confirm the order survived the wrap
    int all[8] = { 0 };
    check("read 8", r.read(all, 8), 8);
    check("read when empty", r.read(out, 1), 0);

    const int want[8] = { 4, 0, 1, 2, 0, 1, 2, 3 };
    long wrong = 0;
    for(int i = 0; i < 8; i++)
        if(all[i] != want[i]) ++wrong;
    check("values in order across the wrap", wrong, 0);

    // short read gives what there is
    check("write 3", r.write(in, 3), 3);
    check("asked 10, got 3", r.read(out, 10), 3);

    check("write 6 then clear", r.write(in, 6), 6);
    r.clear();
    check("empty after clear", r.readable(), 0);
    check("all space back", r.writable(), 8);
}

/**
 * An item that can be checked for having arrived whole.
 *
 * Several words, and the tail derived from the head, so a consumer that sees
 * the item before the producer finished writing it sees a mismatch rather than
 * a plausible value.  Wider than an int on purpose: a single word could be
 * published atomically by accident on some hardware and hide the problem.
 */
struct item {
    int seq;
    int pad[6];
    int derived;

    void fill(int n) {
        seq = n;
        for(int i = 0; i < 6; i++) pad[i] = n + i + 1;
        derived = n * 3 + 7;
    }

    bool whole() const {
        if(derived != seq * 3 + 7) return false;
        for(int i = 0; i < 6; i++)
            if(pad[i] != seq + i + 1) return false;
        return true;
    }
};

static void threaded() {
    std::cout << "\nproducer against consumer:\n";

    // deliberately small relative to the traffic, so both sides really do run
    // out and have to cope with short transfers
    ringbuffer<item> r(64);

    const int total = 2000000;

    long received = 0, torn = 0, out_of_order = 0;

    std::thread producer([&r, total]() {
        int next = 0;
        item chunk[32];
        while(next < total) {
            int n = 0;
            while(n < 32 && next + n < total) { chunk[n].fill(next + n); n++; }

            std::size_t sent = 0;
            while(sent < static_cast<std::size_t>(n))
                sent += r.write(chunk + sent, n - sent);

            next += n;
        }
    });

    std::thread consumer([&r, total, &received, &torn, &out_of_order]() {
        item chunk[32];
        while(received < total) {
            const std::size_t n = r.read(chunk, 32);
            for(std::size_t i = 0; i < n; i++) {
                if(!chunk[i].whole()) ++torn;
                if(chunk[i].seq != static_cast<int>(received)) ++out_of_order;
                ++received;
            }
        }
    });

    producer.join();
    consumer.join();

    check("items received", received, total);
    check("items published before they were written", torn, 0);
    check("items out of order", out_of_order, 0);
    check("empty at the end", r.readable(), 0);
}

int main() {
    single_threaded();
    threaded();

    return failures ? 1 : 0;
}
