// Dump the final-position logits for one forward pass, for jlib #184.
//
// Takes TEXT, prints the ids it tokenized to, and writes the full logit
// vector as raw float32.  The ids are printed because the jlib side is fed
// the *ids*, not the text: jlib's tokenizer is verified separately against
// the same reference, and feeding ids keeps tokenization out of a comparison
// that is about the forward pass.
//
// CPU only (-ngl 0) by default, because a reference wants to be reproducible
// before it wants to be fast.

#include "llama.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if(argc < 4) {
        fprintf(stderr, "usage: %s <model.gguf> <out.bin> <text> [ngl]\n", argv[0]);
        return 2;
    }

    const std::string model_path = argv[1];
    const std::string out_path   = argv[2];
    const std::string text       = argv[3];
    const int         ngl        = argc > 4 ? atoi(argv[4]) : 0;

    llama_backend_init();

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = ngl;

    llama_model* model = llama_model_load_from_file(model_path.c_str(), mp);
    if(!model) { fprintf(stderr, "cannot load %s\n", model_path.c_str()); return 1; }

    const llama_vocab* vocab = llama_model_get_vocab(model);

    std::vector<llama_token> ids;

    // "ids:1,2,3" feeds the forward pass directly, which is what a comparison
    // against another implementation wants: two implementations have to be
    // given the *same* input, and the tokenizer is checked separately.
    if(text.rfind("ids:", 0) == 0) {
        const char* p = text.c_str() + 4;

        while(*p) {
            ids.push_back(llama_token(strtol(p, nullptr, 10)));
            while(*p && *p != ',') p++;
            if(*p == ',') p++;
        }
    }
    else {
        // add_special = true, parse_special = true -- the same convention the
        // tokenizer fixtures were recorded under.
        const int n = -llama_tokenize(vocab, text.c_str(), int(text.size()),
                                      nullptr, 0, true, true);
        ids.resize(size_t(n < 0 ? 0 : n));
        if(llama_tokenize(vocab, text.c_str(), int(text.size()),
                          ids.data(), int(ids.size()), true, true) < 0) {
            fprintf(stderr, "tokenize failed\n");
            return 1;
        }
    }

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx   = uint32_t(ids.size() + 8);
    cp.n_batch = uint32_t(ids.size());
    cp.no_perf = true;

    // **Off, and the comparison is meaningless without it.**  The default is
    // true, which keeps a full-size cache for the sliding-window layers; with
    // it the reference attends to everything and a windowed implementation
    // looks wrong against it (#181).
    cp.swa_full = false;

    llama_context* ctx = llama_init_from_model(model, cp);
    if(!ctx) { fprintf(stderr, "no context\n"); return 1; }

    llama_batch batch = llama_batch_get_one(ids.data(), int32_t(ids.size()));

    if(llama_decode(ctx, batch) != 0) { fprintf(stderr, "decode failed\n"); return 1; }

    const int n_vocab = llama_vocab_n_tokens(vocab);
    const float* logits = llama_get_logits_ith(ctx, int32_t(ids.size()) - 1);
    if(!logits) { fprintf(stderr, "no logits\n"); return 1; }

    FILE* f = fopen(out_path.c_str(), "wb");
    if(!f) { fprintf(stderr, "cannot write %s\n", out_path.c_str()); return 1; }
    fwrite(logits, sizeof(float), size_t(n_vocab), f);
    fclose(f);

    printf("model %s\n", model_path.c_str());
    printf("ngl %d\n", ngl);
    printf("n_vocab %d\n", n_vocab);
    printf("ids %zu:", ids.size());
    for(llama_token id : ids) printf(" %d", id);
    printf("\n");

    // A quick look, so a wrong run is obvious without reading the binary.
    int top = 0;
    for(int i = 1; i < n_vocab; i++) if(logits[i] > logits[top]) top = i;
    char buf[256];
    const int len = llama_token_to_piece(vocab, top, buf, sizeof(buf), 0, true);
    printf("argmax %d logit %.6f piece '%.*s'\n",
           top, double(logits[top]), len > 0 ? len : 0, buf);

    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
