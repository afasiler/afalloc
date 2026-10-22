/* A game-loop proxy: what a simulation typically does with the heap every frame.
 *   spatial hash rebuilt each frame, A* pathfinding queries, a stream of polymorphic events with string
 *   messages, a particle list, plus plain per-entity math. 450 frames; frame time statistics are reported
 *   because games care about the worst frames, not only the average. */
#include "rw_new.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <list>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

static uint64_t rs = 88172645463325252ull;
static inline uint64_t rnd() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static inline float frand() { return (float)(rnd() >> 40) / (float)(1 << 24); }

struct Entity { float x, y, vx, vy; int hp; };
static const int NENT = 20000, GW = 96, CELL = 64;
static bool blocked[GW][GW];

struct Node { int idx, g, f; bool operator<(const Node &o) const { return f > o.f; } };
static int astar(int s, int t, std::vector<int> &path)
{
    std::priority_queue<Node> open;
    std::unordered_map<int, int> came, gs;
    open.push({s, 0, 0});
    gs[s] = 0;
    int tx = t % GW, ty = t / GW, expanded = 0;
    while (!open.empty() && expanded < 4000) {
        Node n = open.top(); open.pop();
        if (n.idx == t) break;
        expanded++;
        int x = n.idx % GW, y = n.idx / GW;
        static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; k++) {
            int nx = x + dx[k], ny = y + dy[k];
            if (nx < 0 || ny < 0 || nx >= GW || ny >= GW || blocked[ny][nx]) continue;
            int ni = ny * GW + nx, ng = n.g + 1;
            auto it = gs.find(ni);
            if (it != gs.end() && it->second <= ng) continue;
            gs[ni] = ng;
            came[ni] = n.idx;
            open.push({ni, ng, ng + std::abs(nx - tx) + std::abs(ny - ty)});
        }
    }
    path.clear();
    for (int c = t; came.count(c) && c != s; c = came[c]) path.push_back(c);
    return (int)path.size();
}

struct Event {
    std::string msg;
    explicit Event(std::string m) : msg(std::move(m)) {}
    virtual ~Event() = default;
    virtual int apply(std::vector<Entity> &e) = 0;
};
struct Damage : Event { int who, amt; Damage(int w, int a) : Event("damage to entity #" + std::to_string(w) + " from the environment"), who(w), amt(a) {} int apply(std::vector<Entity> &e) override { e[who].hp -= amt; return (int)msg.size(); } };
struct Heal : Event { int who; Heal(int w) : Event("heal applied to entity #" + std::to_string(w) + " by a nearby ally"), who(w) {} int apply(std::vector<Entity> &e) override { e[who].hp += 3; return (int)msg.size(); } };
struct Push : Event { int who; Push(int w) : Event("knock-back on #" + std::to_string(w)), who(w) {} int apply(std::vector<Entity> &e) override { e[who].vx += 0.5f; return (int)msg.size(); } };

struct Particle { float x, y, vx, vy; int life; };

int main()
{
    std::vector<Entity> ents(NENT);
    for (auto &e : ents) e = {frand() * 4096, frand() * 4096, frand() - 0.5f, frand() - 0.5f, 100};
    for (int y = 0; y < GW; y++) for (int x = 0; x < GW; x++) blocked[y][x] = (rnd() % 100) < 22;
    std::list<Particle> particles;
    const int FRAMES = 450, WARM = 40;
    std::vector<double> frame_ms;
    frame_ms.reserve(FRAMES);
    double check = 0, total0 = now_ms();
    std::vector<int> path;

    for (int f = 0; f < FRAMES + WARM; f++) {
        double t0 = now_ms();
        std::unordered_map<uint32_t, std::vector<uint32_t>> grid;           /* 1: spatial hash, rebuilt */
        for (int i = 0; i < NENT; i++)
            grid[((uint32_t)(ents[i].x / CELL) << 16) | (uint32_t)(ents[i].y / CELL)].push_back((uint32_t)i);
        for (int q = 0; q < 400; q++) {                                     /* neighbour queries */
            const Entity &e = ents[rnd() % NENT];
            std::vector<uint32_t> near;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                auto it = grid.find((((uint32_t)(e.x / CELL) + dx) << 16) | ((uint32_t)(e.y / CELL) + dy));
                if (it != grid.end()) near.insert(near.end(), it->second.begin(), it->second.end());
            }
            check += (double)near.size();
        }
        for (int q = 0; q < 30; q++) {                                      /* 2: pathfinding */
            int s = (int)(rnd() % (GW * GW)), t = (int)(rnd() % (GW * GW));
            if (!blocked[s / GW][s % GW] && !blocked[t / GW][t % GW]) check += astar(s, t, path);
        }
        std::vector<std::unique_ptr<Event>> events;                         /* 3: events */
        for (int k = 0; k < 2500; k++) {
            int who = (int)(rnd() % NENT);
            switch (rnd() % 3) {
                case 0: events.push_back(std::make_unique<Damage>(who, 1 + (int)(rnd() % 5))); break;
                case 1: events.push_back(std::make_unique<Heal>(who)); break;
                default: events.push_back(std::make_unique<Push>(who)); break;
            }
        }
        for (auto &ev : events) check += ev->apply(ents);
        events.clear();
        for (int k = 0; k < 1200; k++) particles.push_back({frand() * 4096, frand() * 4096, frand() - 0.5f, frand() - 0.5f, 10 + (int)(rnd() % 30)});   /* 4: particles */
        for (auto it = particles.begin(); it != particles.end();) {
            it->x += it->vx; it->y += it->vy;
            if (--it->life <= 0) it = particles.erase(it); else ++it;
        }
        for (auto &e : ents) {                                              /* 5: plain math */
            e.x += e.vx; e.y += e.vy;
            if (e.x < 0 || e.x > 4096) e.vx = -e.vx;
            if (e.y < 0 || e.y > 4096) e.vy = -e.vy;
        }
        if (f >= WARM) frame_ms.push_back(now_ms() - t0);
    }
    double sum = 0;
    for (double v : frame_ms) sum += v;
    std::sort(frame_ms.begin(), frame_ms.end());
    metric("game_frame_avg_ms", sum / (double)frame_ms.size());
    metric("game_frame_p50_ms", frame_ms[frame_ms.size() / 2]);
    metric("game_frame_p99_ms", frame_ms[frame_ms.size() * 99 / 100]);
    metric("game_frame_max_ms", frame_ms.back());
    metric("game_total_ms", now_ms() - total0);
    metric("checksum", check + (double)particles.size());
    return 0;
}
