/* An AI proxy on the CPU.
 *   infer     a 4-layer MLP, batch 8; every inference allocates its activation tensors with new[]
 *             (about 64 KiB to 128 KiB each): compute bound
 *   tokenize  a data-pipeline step: split text into words, map words to ids with a growing vocabulary, build
 *             padded id batches: thousands of small strings and vectors */
#include "rw_new.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

static uint64_t rs = 88172645463325252ull;
static inline uint64_t rnd() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static inline float frand() { return (float)(rnd() >> 40) / (float)(1 << 24) - 0.5f; }

struct Layer { int in, out; std::vector<float> w, b; };

static float *forward(const Layer &L, const float *x, int batch)
{
    float *y = new float[(size_t)batch * L.out];                          /* the activation tensor of this layer */
    for (int n = 0; n < batch; n++)
        for (int o = 0; o < L.out; o++) {
            const float *w = &L.w[(size_t)o * L.in];
            float acc = L.b[o];
            for (int i = 0; i < L.in; i++) acc += w[i] * x[(size_t)n * L.in + i];
            y[(size_t)n * L.out + o] = acc > 0 ? acc : 0;
        }
    return y;
}

int main()
{
    const int dims[5] = {768, 1024, 1024, 768, 256}, batch = 8;
    std::vector<Layer> net;
    for (int l = 0; l < 4; l++) {
        Layer L{dims[l], dims[l + 1], {}, {}};
        L.w.resize((size_t)L.in * L.out); L.b.resize(L.out);
        for (auto &v : L.w) v = frand() * 0.05f;
        for (auto &v : L.b) v = frand() * 0.01f;
        net.push_back(std::move(L));
    }
    std::vector<float> input((size_t)batch * dims[0]);
    for (auto &v : input) v = frand();
    const int RUNS = 150;
    std::vector<double> ms;
    double sink = 0;
    for (int r = 0; r < RUNS + 10; r++) {
        double t0 = now_ms();
        float *x = new float[input.size()];
        std::copy(input.begin(), input.end(), x);
        for (int l = 0; l < 4; l++) { float *y = forward(net[l], x, batch); delete[] x; x = y; }
        std::vector<float> out(x, x + (size_t)batch * dims[4]);            /* copy the result out, as a framework would */
        delete[] x;
        for (float v : out) sink += v;
        if (r >= 10) ms.push_back(now_ms() - t0);
    }
    double s = 0;
    for (double v : ms) s += v;
    std::sort(ms.begin(), ms.end());
    metric("nn_infer_avg_ms", s / (double)ms.size());
    metric("nn_infer_p99_ms", ms[ms.size() * 99 / 100]);

    /* ---- tokenizer / data pipeline ---- */
    std::vector<std::string> vocab_words;
    for (int i = 0; i < 20000; i++) {
        std::string w;
        int len = 3 + (int)(rnd() % 8);
        for (int k = 0; k < len; k++) w += (char)('a' + rnd() % 26);
        vocab_words.push_back(w);
    }
    std::vector<std::string> lines;
    for (int i = 0; i < 80000; i++) {
        std::string line;
        int n = 8 + (int)(rnd() % 12);
        for (int k = 0; k < n; k++) { if (k) line += ' '; line += vocab_words[(rnd() % 20000) % (1 + rnd() % 20000)]; }
        lines.push_back(std::move(line));
    }
    double t0 = now_ms();
    size_t tokens = 0;
    for (int epoch = 0; epoch < 3; epoch++) {
        std::unordered_map<std::string, int> vocab;
        std::vector<std::vector<int>> batches;
        for (const std::string &raw : lines) {
            std::string lower = raw;
            for (char &c : lower) c = (char)std::tolower((unsigned char)c);
            std::vector<std::string> words;
            size_t p = 0;
            while (p < lower.size()) {
                size_t q = lower.find(' ', p);
                if (q == std::string::npos) q = lower.size();
                words.emplace_back(lower, p, q - p);
                p = q + 1;
            }
            std::vector<int> ids;
            for (const std::string &w : words) {
                auto it = vocab.find(w);
                if (it == vocab.end()) it = vocab.emplace(w, (int)vocab.size()).first;
                ids.push_back(it->second);
            }
            ids.resize(32, 0);
            tokens += ids.size();
            batches.push_back(std::move(ids));
        }
        sink += (double)batches.size() + (double)vocab.size();
    }
    metric("nn_tokenize_ms", now_ms() - t0);
    metric("checksum", sink + (double)tokens);
    return 0;
}
