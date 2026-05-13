#include <vector>
#include <unordered_map>
#include <optional>
#include <stdexcept>

using namespace std;

struct LockEdge {
    int thread_id;
    int holds_lock_id;
    int waits_for_lock_id;
};

enum class Color {
    WHITE,
    GRAY,
    BLACK
};

static bool DFS(int u, const vector<vector<int>> &adj, vector<Color> &color, vector<int> &parent, int &cycle_start) {
    color[u] = Color::GRAY;

    for (int v : adj[u]) {
        if (color[v] == Color::GRAY) {
            cycle_start = v;
            if (parent[v] == -1)
                parent[v] = u;
            return true;
        }
        if (color[v] == Color::WHITE) {
            parent[v] = u;
            DFS(v, adj, color, parent, cycle_start);
        }
    }
    color[u] = Color::BLACK;
    return false;
}

bool detect_deadlock(const vector<LockEdge> &edges, vector<int> &cycle) {
    cycle.clear();

    unordered_map<int, int> holder;
    for (const auto &e : edges) {
        if (e.holds_lock_id >= 0) {
            holder[e.holds_lock_id] = e.thread_id;
        }
    }

    int V = 0;
    for (const auto &e : edges) {
        V = max(V, e.thread_id + 1);
    }

    vector<vector<int>> adj(V);
    for (const auto &e : edges) {
        if (e.waits_for_lock_id < 0) continue;

        auto it = holder.find(e.waits_for_lock_id);
        if (it == holder.end())
            continue;
        if (it->second == e.thread_id)
            continue;

        adj[e.thread_id].push_back(it->second);
    }

    vector<Color> color(V, Color::WHITE);
    vector<int>   parent(V, -1);
    int           cycle_start = -1;

    bool found = false;
    for (int u = 0; u < V && !found; ++u) {
        if (color[u] == Color::WHITE)
            found = DFS(u, adj, color, parent, cycle_start);
    }

    if (!found)
        return false;

    cycle.reserve(V);
    int cur = cycle_start;
    do {
        cycle.push_back(cur);
        cur = parent[cur];
    } while (cur != cycle_start && cur != -1);

    cycle.push_back(cycle_start);
    return true;
}