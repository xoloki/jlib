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
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace ai = jlib::ai;

namespace {

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

/**
 * One exchange: lay out the turns, generate, stream the reply to stdout.
 *
 * @return the reply, so the caller can put it in the history
 */
template<typename T>
std::string answer(ai::model<T>& m, ai::backend<T>& b, const ai::tokenizer& tok,
                   const std::vector<int>& ids, const options& o,
                   ai::sampler& s)
{
    std::string reply;
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

            reply += p;

            std::cout << p << std::flush;
        });

    std::cout << "\n";

    const double took = now() - start;
    const std::size_t made = out.size() - ids.size();

    std::cerr << "[" << made << " tokens in " << took << "s, "
              << (took > 0 ? double(made) / took : 0.0) << "/s]\n";

    return reply;
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

        answer(m, b, tok, ids, o, s);

        return 0;
    }

    if(o.raw) {
        std::cerr << "jchat: --raw needs a prompt on the command line\n";

        return 1;
    }

    std::cerr << "[type a message, or end input to stop]\n";

    for(;;) {
        std::cerr << "> " << std::flush;

        std::string line;

        if(!std::getline(std::cin, line)) break;

        if(line.empty()) continue;

        turns.push_back({ "user", line });

        fit(turns, *ch, tok, c.context, o.tokens);

        const std::string said =
            answer(m, b, tok, ch->encode(turns, tok), o, s);

        turns.push_back({ "assistant", said });
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
