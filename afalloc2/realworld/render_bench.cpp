/* A graphics proxy on the CPU: asset loading, BVH construction and ray tracing.
 *   load    parse an OBJ-like text of 200,000 triangles line by line with istringstream (allocation heavy)
 *   build   build a pointer-based BVH (one new per node, a vector of triangle ids per leaf)
 *   render  640x360 primary and shadow rays with an explicit stack (no allocation: compute bound)
 *   render_naive  the same, but each ray collects its hits in a fresh std::vector, as toy tracers do */
#include "rw_new.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

static uint64_t rs = 88172645463325252ull;
static inline uint64_t rnd() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static inline float frand() { return (float)(rnd() >> 40) / (float)(1 << 24); }

struct Tri { float v[3][3]; };
struct Node { float lo[3], hi[3]; Node *l = nullptr, *r = nullptr; std::vector<int> tris; };
static std::vector<Tri> tris;

static Node *build(std::vector<int> &idx, int lo, int hi)
{
    Node *n = new Node;
    for (int a = 0; a < 3; a++) { n->lo[a] = 1e30f; n->hi[a] = -1e30f; }
    for (int i = lo; i < hi; i++)
        for (int k = 0; k < 3; k++) for (int a = 0; a < 3; a++) {
            n->lo[a] = std::min(n->lo[a], tris[idx[i]].v[k][a]);
            n->hi[a] = std::max(n->hi[a], tris[idx[i]].v[k][a]);
        }
    if (hi - lo <= 4) { n->tris.assign(idx.begin() + lo, idx.begin() + hi); return n; }
    int axis = 0;
    for (int a = 1; a < 3; a++) if (n->hi[a] - n->lo[a] > n->hi[axis] - n->lo[axis]) axis = a;
    int mid = (lo + hi) / 2;
    std::nth_element(idx.begin() + lo, idx.begin() + mid, idx.begin() + hi, [axis](int a, int b) {
        return tris[a].v[0][axis] + tris[a].v[1][axis] + tris[a].v[2][axis] < tris[b].v[0][axis] + tris[b].v[1][axis] + tris[b].v[2][axis]; });
    n->l = build(idx, lo, mid);
    n->r = build(idx, mid, hi);
    return n;
}
static void destroy(Node *n) { if (!n) return; destroy(n->l); destroy(n->r); delete n; }

static inline bool hit_box(const Node *n, const float *o, const float *inv, float tmax)
{
    float t0 = 0, t1 = tmax;
    for (int a = 0; a < 3; a++) {
        float a0 = (n->lo[a] - o[a]) * inv[a], a1 = (n->hi[a] - o[a]) * inv[a];
        if (a0 > a1) std::swap(a0, a1);
        t0 = std::max(t0, a0); t1 = std::min(t1, a1);
        if (t0 > t1) return false;
    }
    return true;
}
static inline float hit_tri(const Tri &t, const float *o, const float *d)       /* Moller-Trumbore, -1 if no hit */
{
    float e1[3], e2[3], p[3], s[3], q[3];
    for (int a = 0; a < 3; a++) { e1[a] = t.v[1][a] - t.v[0][a]; e2[a] = t.v[2][a] - t.v[0][a]; }
    p[0] = d[1] * e2[2] - d[2] * e2[1]; p[1] = d[2] * e2[0] - d[0] * e2[2]; p[2] = d[0] * e2[1] - d[1] * e2[0];
    float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (std::fabs(det) < 1e-9f) return -1;
    float inv = 1.0f / det;
    for (int a = 0; a < 3; a++) s[a] = o[a] - t.v[0][a];
    float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
    if (u < 0 || u > 1) return -1;
    q[0] = s[1] * e1[2] - s[2] * e1[1]; q[1] = s[2] * e1[0] - s[0] * e1[2]; q[2] = s[0] * e1[1] - s[1] * e1[0];
    float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
    if (v < 0 || u + v > 1) return -1;
    float tt = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
    return tt > 1e-4f ? tt : -1;
}
static float trace(const Node *root, const float *o, const float *d, bool want_all, std::vector<float> *hits)
{
    float inv[3] = {1.0f / (std::fabs(d[0]) < 1e-12f ? 1e-12f : d[0]), 1.0f / (std::fabs(d[1]) < 1e-12f ? 1e-12f : d[1]), 1.0f / (std::fabs(d[2]) < 1e-12f ? 1e-12f : d[2])};
    const Node *stack[96]; int sp = 0;
    stack[sp++] = root;
    float best = 1e30f;
    while (sp) {
        const Node *n = stack[--sp];
        if (!hit_box(n, o, inv, want_all ? 1e30f : best)) continue;
        if (!n->l) {
            for (int i : n->tris) {
                float t = hit_tri(tris[i], o, d);
                if (t > 0) { if (want_all) hits->push_back(t); else if (t < best) best = t; }
            }
        } else { stack[sp++] = n->l; stack[sp++] = n->r; }
    }
    return best;
}

int main()
{
    const int NT = 200000;
    std::string obj;                                                     /* asset text, generated once */
    obj.reserve((size_t)NT * 70);
    for (int i = 0; i < NT; i++) {
        float cx = frand() * 100, cy = frand() * 100, cz = frand() * 100;
        char line[128];
        std::snprintf(line, sizeof line, "f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n", cx, cy, cz, cx + frand(), cy + frand(), cz + frand(), cx + frand(), cy + frand(), cz + frand());
        obj += line;
    }
    double t0 = now_ms();
    tris.reserve(NT);
    std::istringstream all(obj);                                         /* load: line by line, a stream per line */
    std::string line;
    while (std::getline(all, line)) {
        std::istringstream ls(line);
        std::string tag;
        Tri t;
        ls >> tag;
        for (int k = 0; k < 3; k++) for (int a = 0; a < 3; a++) ls >> t.v[k][a];
        tris.push_back(t);
    }
    metric("render_load_ms", now_ms() - t0);

    t0 = now_ms();
    std::vector<int> idx(tris.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = (int)i;
    Node *root = build(idx, 0, (int)idx.size());
    metric("render_build_ms", now_ms() - t0);

    const int W = 640, H = 360;
    double sum = 0;
    t0 = now_ms();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        float o[3] = {50, 50, -60}, d[3] = {((float)x / W - 0.5f) * 1.2f, ((float)y / H - 0.5f) * 0.7f, 1.0f};
        float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int a = 0; a < 3; a++) d[a] /= len;
        float t = trace(root, o, d, false, nullptr);
        if (t < 1e29f) {
            float p[3] = {o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t}, ld[3] = {0.3f, 0.8f, -0.5f};
            float ll = std::sqrt(ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2]);
            for (int a = 0; a < 3; a++) ld[a] /= ll;
            sum += trace(root, p, ld, false, nullptr) < 1e29f ? 0.2 : 1.0;
        }
    }
    metric("render_fast_ms", now_ms() - t0);

    t0 = now_ms();
    double sum2 = 0;
    for (int y = 0; y < H; y += 2) for (int x = 0; x < W; x += 2) {
        float o[3] = {50, 50, -60}, d[3] = {((float)x / W - 0.5f) * 1.2f, ((float)y / H - 0.5f) * 0.7f, 1.0f};
        float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int a = 0; a < 3; a++) d[a] /= len;
        std::vector<float> hits;                                         /* a new vector per ray */
        trace(root, o, d, true, &hits);
        std::sort(hits.begin(), hits.end());
        sum2 += hits.empty() ? 0 : hits[0];
    }
    metric("render_naive_ms", now_ms() - t0);
    metric("checksum", sum + sum2);
    destroy(root);
    return 0;
}
