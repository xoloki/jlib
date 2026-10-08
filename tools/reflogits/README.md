# Reference logits

Regenerates `tests/reference_logits.hh`, the llama.cpp fixture that
`ai_model_test` compares jlib's forward pass against (#184).

**Not built by `make`, and deliberately not a SUBDIR.** It needs a llama.cpp
checkout, and jlib does not depend on llama.cpp — the comparison is recorded
in the tree precisely so that running the tests does not.

## Why llama.cpp and not transformers

It reads **the same GGUF file**. #180 found that `modeling_gemma2.py`
describes the HuggingFace checkpoint while the GGUF is a different artefact,
so an oracle built from HF weights can carry the error it is meant to catch.

## Regenerating

```sh
# 1. build llama.cpp (once)
cd ~/src/llama.cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j10 --target llama common

# 2. build this
L=~/src/llama.cpp
c++ -std=c++17 -O2 -I$L/include -I$L/ggml/include \
    tools/reflogits/dump_logits.cpp \
    -L$L/build/bin -lllama -lggml -lggml-base -o /tmp/dump_logits

# 3. dump each architecture, on CPU
cd <where the GGUFs are>
for m in tinyllama-1.1b-chat-v1.0.Q8_0 qwen2.5-0.5b-instruct-q8_0 \
         gemma-2-2b-it-Q8_0; do
    DYLD_LIBRARY_PATH=$L/build/bin /tmp/dump_logits \
        $m.gguf /tmp/$m.bin "The capital of France is" 0
done

# 4. emit the header -- the first argument is the llama.cpp commit, which
#    goes into the fixture so a disagreement can be attributed
python3 tools/reflogits/emit_header.py $(git -C $L rev-parse --short HEAD) \
    /tmp/tinyllama-1.1b-chat-v1.0.Q8_0.bin \
    /tmp/qwen2.5-0.5b-instruct-q8_0.bin \
    /tmp/gemma-2-2b-it-Q8_0.bin > tests/reference_logits.hh
```

`dump_logits` prints the token ids it used. **They are the input to the
comparison**, and `emit_header.py` carries them hardcoded — if a vendor
reships a tokenizer and the ids change, update both or the fixture will
describe a prompt the test does not run.

## Two settings that decide whether the comparison means anything

**`swa_full = false`.** `llama_context_default_params()` sets it true, which
keeps a full-size cache for sliding-window layers. The dump program sets it
false so the window is actually enforced; with the default a windowed
implementation looks wrong against the reference (#181).

**`ids:1,2,3` instead of text.** Feeds the forward pass a token list directly,
so both implementations are given the identical input and tokenization is out of
a comparison that is about arithmetic. `dump_logits` prints the ids it used, so
the usual flow is one text run to get them and `ids:` runs thereafter.

## CPU, not Metal

`-ngl 0`. A reference wants to be reproducible before it wants to be fast,
and jlib's side of the comparison is its host backend for the same reason.
The Metal backend is checked against the host elsewhere.

## If the comparison starts failing

Check whether llama.cpp moved first. The commit that produced the fixture is
in the header; a kernel change on their side is a reason to regenerate, and a
change on ours is a regression. Raising the tolerance is neither.
