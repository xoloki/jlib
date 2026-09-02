/* -*- mode: C++ c-basic-offset: 4  -*-
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

/**
 * jchat -- talk to a GGUF model.
 *
 * Two modes, one program. With a prompt on the command line it answers once
 * and exits, which is what a script or a test wants; with none it reads turns
 * from standard input and keeps the conversation, which is what a person
 * wants.
 *
 * Progress and timings go to **stderr** and the model's words to **stdout**,
 * so `jchat model.gguf "question" > answer.txt` collects the answer and
 * nothing else.
 */

#include <jlib/ai/chat.hh>
#include <jlib/ai/generate.hh>
#include <jlib/ai/model.hh>
#include <jlib/ai/tokenizer.hh>

#ifdef HAVE_METAL
#include <jlib/metal/backend.hh>
#endif

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace ai = jlib::ai;

namespace {

/**
 * Set by the interrupt handler, read between tokens.
 *
 * sig_atomic_t and volatile because a handler may run between any two
 * instructions, and nothing else in here is safe to touch from one -- no
 * allocation, no iostreams, no locks. The handler sets a number; everything
 * that has to happen because of it happens in the generation loop, where it is
 * allowed to.
 */
volatile std::sig_atomic_t g_interrupted = 0;

void interrupted(int) { g_interrupted = 1; }

/**
 * Ask for SIGINT **without** SA_RESTART.
 *
 * signal(2) on a BSD restarts an interrupted read, which is usually what you
 * want and here is not: with it, Ctrl-C at the prompt would be noticed only
 * after the next line was typed. sigaction with no flags lets getline fail so
 * the loop can see it.
 */
void catch_interrupts() {
    struct sigaction sa;

    std::memset(&sa, 0, sizeof(sa));

    sa.sa_handler = interrupted;

    sigemptyset(&sa.sa_mask);

    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, 0);
}

struct options {
    std::string model;
    std::string prompt;
    std::string system;

    unsigned int tokens = 256;

    ai::sampler::config sampling;

    /** Continue the text rather than laying it out as a conversation. */
    bool raw = false;
};

void usage(std::ostream& o, const char* argv0) {
    o << "usage: " << argv0 << " [options] <model.gguf> [prompt ...]\n"
      << "\n"
      << "  With a prompt, answers it and exits.  Without one, reads a turn\n"
      << "  per line from stdin and keeps the conversation going.\n"
      << "\n"
      << "  --system TEXT   a system turn before the conversation\n"
      << "  --tokens N      most tokens in one reply (default 256)\n"
      << "  --temp F        0 is greedy and repeatable (default 0.8)\n"
      << "  --top-k N       keep the N likeliest, 0 for all (default 40)\n"
      << "  --top-p F       keep the likeliest summing to F (default 0.95)\n"
      << "  --repeat F      discourage a token already seen; 1 is\n"
      << "                  off (default 1). Qwen 2.5 asks for 1.1,\n"
      << "                  Llama 3.2 for none\n"
      << "  --seed N        for a repeatable run at a temperature above 0\n"
      << "  --raw           continue the prompt instead of answering it,\n"
      << "                  which is what a base model wants\n"
      << "  --help\n";
}

bool parse(int argc, char** argv, options& o) {
    int i = 1;

    for(; i < argc; i++) {
        const std::string a = argv[i];

        if(a == "--help" || a == "-h") { usage(std::cout, argv[0]); std::exit(0); }
        else if(a == "--raw") o.raw = true;
        else if(a.size() > 2 && a.compare(0, 2, "--") == 0) {
            if(i + 1 >= argc) {
                std::cerr << "jchat: " << a << " wants a value\n";

                return false;
            }

            const std::string v = argv[++i];

            if(a == "--system") o.system = v;
            else if(a == "--tokens") o.tokens = unsigned(std::atoi(v.c_str()));
            else if(a == "--temp") o.sampling.temperature = float(std::atof(v.c_str()));
            else if(a == "--top-k") o.sampling.top_k = unsigned(std::atoi(v.c_str()));
            else if(a == "--top-p") o.sampling.top_p = float(std::atof(v.c_str()));
            else if(a == "--repeat")
                o.sampling.repetition_penalty = float(std::atof(v.c_str()));
            else if(a == "--seed") o.sampling.seed = std::uint64_t(std::atoll(v.c_str()));
            else {
                std::cerr << "jchat: no such option " << a << "\n";

                return false;
            }
        }
        else break;
    }

    if(i >= argc) {
        usage(std::cerr, argv[0]);

        return false;
    }

    o.model = argv[i++];

    // Everything after the model is the prompt, joined with spaces, so a
    // shell that split it on words has not changed what was asked.
    for(; i < argc; i++) {
        if(!o.prompt.empty()) o.prompt += " ";

        o.prompt += argv[i];
    }

    return true;
}

double now() {
    using namespace std::chrono;

    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/** What one exchange produced, and whether it got to finish. */
struct reply {
    std::string text;
    bool stopped = false;
};

/**
 * One exchange: lay out the turns, generate, stream the reply to stdout.
 *
 * Reports whether it was interrupted rather than leaving the caller to read
 * the flag afterwards -- this clears it, so a caller checking it later would
 * always see zero, which is a mistake this returned a bare string long enough
 * to make.
 */
template<typename T>
reply answer(ai::model<T>& m, ai::backend<T>& b, const ai::tokenizer& tok,
                   const std::vector<int>& ids, const options& o,
                   ai::sampler& s)
{
    reply said;
    bool first = true;

    const double start = now();

    const std::vector<int> out = ai::generate<T>(
        m, b, ids, o.tokens, s, tok.eos(),
        [&](int id) {
            std::string p = tok.piece(id);

            // The first token of a reply carries the space that separated it
            // from the marker before it, which is not part of what was said.
            if(first) {
                while(!p.empty() && p[0] == ' ') p.erase(0, 1);

                first = false;
            }

            said.text += p;

            std::cout << p << std::flush;

            // Between tokens, which is where it is safe to stop: nothing is
            // half-printed and the reply so far is a whole thing.
            return g_interrupted == 0;
        });

    std::cout << "\n";

    const double took = now() - start;
    const std::size_t made = out.size() - ids.size();

    if(g_interrupted) {
        said.stopped = true;

        std::cerr << "[stopped after " << made << " tokens]\n";

        // Cleared here rather than by the handler, so the next reply starts
        // fresh and a second Ctrl-C at the prompt is a separate decision.
        g_interrupted = 0;
    }
    else
        std::cerr << "[" << made << " tokens in " << took << "s, "
                  << (took > 0 ? double(made) / took : 0.0) << "/s]\n";

    return said;
}

/**
 * Drop the oldest turns until the prompt fits.
 *
 * A conversation grows without bound and a context does not. The system turn
 * stays -- it is instructions rather than history, and dropping it changes how
 * the model behaves rather than what it remembers.
 */
void fit(std::vector<ai::message>& turns, const ai::chat& ch,
         const ai::tokenizer& tok, unsigned int context, unsigned int reserve)
{
    for(;;) {
        const std::size_t n = ch.encode(turns, tok).size();

        if(n + reserve <= context) return;

        // Find the oldest turn that is not the system prompt.
        std::size_t at = turns.size();

        for(std::size_t i = 0; i < turns.size(); i++)
            if(turns[i].role != "system") { at = i; break; }

        if(at == turns.size()) return;   // nothing left to give

        std::cerr << "[dropping the oldest turn to stay inside "
                  << context << " tokens]\n";

        turns.erase(turns.begin() + long(at));
    }
}

template<typename T>
int run(ai::backend<T>& b, const ai::gguf& g, const options& o) {
    const ai::tokenizer tok(g);

    typename ai::model<T>::config c = ai::model<T>::config::from(g);

    std::cerr << "[" << b.name() << ", " << c.layers << " layers, "
              << c.d_model << " wide, " << c.heads << " heads over "
              << c.kv_heads << "]\n";

    ai::model<T> m(b, c);

    const double t0 = now();

    m.load(g);

    std::cerr << "[loaded in " << (now() - t0) << "s]\n";

    // Keys and values kept between tokens, so a reply costs one pass per token
    // rather than one over everything so far.  generate() refills it from the
    // prompt each time, which is a single pass and correct however the
    // conversation grew; reusing it across turns would be a further step and
    // needs the new prompt to really be an extension of the old one.
    m.enable_cache();

    catch_interrupts();

    ai::sampler s(o.sampling);

    // Raw mode never lays anything out, so it needs no template -- which is
    // also the escape hatch for a model whose template this cannot read.
    std::shared_ptr<ai::chat> ch;

    if(!o.raw) {
        try { ch.reset(new ai::chat(g, tok.token(tok.eos()))); }
        catch(std::exception& e) {
            std::cerr << "jchat: " << e.what() << "\n"
                      << "jchat: --raw will continue the prompt instead\n";

            return 1;
        }
    }

    std::vector<ai::message> turns;

    if(!o.system.empty()) turns.push_back({ "system", o.system });

    // One shot.
    if(!o.prompt.empty()) {
        std::vector<int> ids;

        if(o.raw) ids = tok.encode(o.prompt);
        else {
            turns.push_back({ "user", o.prompt });
            fit(turns, *ch, tok, c.context, o.tokens);

            // chat::encode, not encode(format(...)): what a user typed must
            // not be able to close the turn and start a new one.
            ids = ch->encode(turns, tok);
        }

        // 128 + SIGINT, which is what a shell reports for a program that was
        // interrupted -- true here even though it was handled rather than
        // fatal, because a script wants to know the answer is not complete.
        return answer(m, b, tok, ids, o, s).stopped ? 130 : 0;
    }

    if(o.raw) {
        std::cerr << "jchat: --raw needs a prompt on the command line\n";

        return 1;
    }

    std::cerr << "[type a message, or end input to stop]\n";

    for(;;) {
        std::cerr << "> " << std::flush;

        std::string line;

        if(!std::getline(std::cin, line)) {
            // Either the input ended or Ctrl-C arrived while waiting for it.
            // Both mean stop; only one of them is an interruption.
            if(g_interrupted) { std::cerr << "\n"; return 130; }

            break;
        }

        if(line.empty()) continue;

        turns.push_back({ "user", line });

        fit(turns, *ch, tok, c.context, o.tokens);

        // Kept whether or not it finished: a stopped reply is still what the
        // model said, and dropping it would leave the conversation with a
        // question nobody answered.
        turns.push_back({ "assistant",
                          answer(m, b, tok, ch->encode(turns, tok), o, s).text });
    }

    std::cerr << "\n";

    return 0;
}

}

int main(int argc, char** argv) {
    options o;

    if(!parse(argc, argv, o)) return 2;

    try {
        const ai::gguf g(o.model);

#ifdef HAVE_METAL
        // fp16 on the GPU, which halves the weights and is what MPS is built
        // for -- and whose GEMM accumulates wider than it stores.
        try {
            jlib::metal::backend<_Float16> b;

            return run<_Float16>(b, g, o);
        }
        catch(std::exception& e) {
            std::cerr << "jchat: no Metal backend (" << e.what() << ")\n";
        }
#endif

        // float, not fp16: the host GEMM accumulates in the element type, and
        // a 2048-long dot product summed in fp16 loses the answer rather than
        // some precision.  Correct, and slow enough to say so.
        std::cerr << "jchat: running on the CPU in float -- this will be very "
                  << "slow and will want about 4GB\n";

        ai::host_backend<float> b;

        return run<float>(b, g, o);
    }
    catch(std::exception& e) {
        std::cerr << "jchat: " << e.what() << "\n";

        return 1;
    }
}
