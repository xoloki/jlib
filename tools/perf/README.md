# tools/perf

What a change to the compute path costs, measured. Built only with `--enable-perf` **and** a Metal device, never by `make check`
— every one of them measures the GPU path.

    ../configure --enable-perf && make
    ./tools/perf/jperf-quick ~/models/tinyllama-1.1b-chat-v1.0.Q8_0.gguf

| | what it answers |
| --- | --- |
| `jperf-quick` | decode, a short prefill and a long one — fast enough to iterate on |
| `jperf-bench` | decode per layer, decode at depth against a cache, prefill at length |
| `jperf-ops` | every backend kernel, alone, at the shapes one layer uses while decoding |
| `jperf-bwmax` | what the device will actually move: scalar copy, vector copy, read-only |
| `jperf-counters` | which counter sets the device exposes to a profiler |

## Why these are in the tree

Every performance number in #158, #163, #187, #189, #190, #191, #192 and #193
came from these programs while they lived outside the repository, unreviewed.
One of them was wrong by a factor of fifteen: `ops` timed a single cold batch
and reported `rms_norm` at 213 us, where the same unchanged kernel reads
13.8 us with warm-up and best-of-batches. A merge decision rested on it. See
#197.

## How to read them

**Take the minimum, never the mean.** `ops` runs fifty warm-up calls and the
best of five batches for exactly this reason — the same kernel measured 4.6,
22.1 and 16.0 us across three consecutive runs of the version that averaged
one cold batch. The minimum is the least contaminated estimate of what a
kernel costs; everything above it is some other load.

**Discard `jperf-bwmax`'s first run.** Cold it reports 91-108 GB/s; warm it
settles at 129-130 for a half4 copy and 133-135 read-only, run after run.

**`ops` does not sum to a step.** The kernels are run one at a time and a real
step overlaps them, so the sum is an upper bound. On an M5 with TinyLlama the
sum is ~365 us against a real layer of ~464 us, and that gap is #194.

**A ceiling measured with your own code is a measurement of your own code.**
#190 used jlib's `add_scaled` at 80 GB/s as a hardware ceiling and concluded
there was 1.7x available. `jperf-bwmax` exists because the device does 131 and
the real figure was 2.8x.

## Memory

`jperf-bench` at 2048 positions holds about 1.7 GB. Before the shared scratch
in #188 the same run held ~7 GB and 2048 failed outright. These carry no RSS
guard — unguarded probes of this kind contributed to exhausting the machine on
2026-09-02, a day after the watchdog panic in #171.
