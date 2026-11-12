#include "RoutingGraph.h"

#include <cmath>
#include <functional>
#include <queue>

namespace mix {

namespace {
// Source ids occupy the low range, bus ids start above this. Keeping
// separate ranges lets one key space address both kinds of node.
constexpr unsigned kBusIdBase = 1000;

float dbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}
}  // namespace

unsigned RoutingGraph::addSource(SourceKind kind, std::string name,
                                 int channels, int remoteIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    SourceInfo s;
    s.id = static_cast<unsigned>(sources_.size());
    s.kind = kind;
    s.name = std::move(name);
    s.channels = channels;
    s.remoteIndex = remoteIndex;
    sources_.push_back(std::move(s));
    return sources_.back().id;
}

unsigned RoutingGraph::addBus(BusKind kind, std::string name, int channels,
                              int remoteIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    BusInfo b;
    b.id = kBusIdBase + static_cast<unsigned>(buses_.size());
    b.kind = kind;
    b.name = std::move(name);
    b.channels = channels;
    b.remoteIndex = remoteIndex;
    buses_.push_back(std::move(b));
    return buses_.back().id;
}

bool RoutingGraph::isBusNode(unsigned id) const { return id >= kBusIdBase; }

bool RoutingGraph::nodeExists(unsigned id) const {
    if (isBusNode(id)) return (id - kBusIdBase) < buses_.size();
    return id < sources_.size();
}

bool RoutingGraph::reachable(
    unsigned from, unsigned to,
    const std::vector<std::vector<unsigned>>& adj) const {
    // BFS; graph is small, and every mutation only needs one search.
    std::vector<unsigned char> seen(kBusIdBase + buses_.size(), 0);
    std::queue<unsigned> q;
    q.push(from);
    seen[from] = 1;
    while (!q.empty()) {
        unsigned cur = q.front();
        q.pop();
        if (cur == to) return true;
        if (cur >= adj.size()) continue;
        for (unsigned nxt : adj[cur]) {
            if (nxt < seen.size() && !seen[nxt]) {
                seen[nxt] = 1;
                q.push(nxt);
            }
        }
    }
    return false;
}

bool RoutingGraph::createsCycle(unsigned from, unsigned to) const {
    // Caller holds mutex_. A new edge from->to closes a loop exactly when
    // to can already reach from (including to == from).
    if (!nodeExists(from) || !nodeExists(to)) return true;
    if (!isBusNode(to)) return true;  // sources have no inputs
    if (from == to) return true;

    std::vector<std::vector<unsigned>> adj(kBusIdBase + buses_.size());
    for (const auto& entry : edgeMap_) {
        unsigned a = static_cast<unsigned>(entry.first >> 32);
        unsigned b = static_cast<unsigned>(entry.first & 0xffffffffu);
        if (a >= adj.size()) adj.resize(a + 1);
        adj[a].push_back(b);
    }
    return reachable(to, from, adj);
}

bool RoutingGraph::setEdge(unsigned from, unsigned to, EdgeParams params) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (createsCycle(from, to)) return false;
    edgeMap_[key(from, to)] = std::move(params);
    return true;
}

bool RoutingGraph::updateEdge(unsigned from, unsigned to,
                              EdgeParams params) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = edgeMap_.find(key(from, to));
    if (it == edgeMap_.end()) return false;
    it->second = std::move(params);
    return true;
}

bool RoutingGraph::removeEdge(unsigned from, unsigned to) {
    std::lock_guard<std::mutex> lock(mutex_);
    return edgeMap_.erase(key(from, to)) > 0;
}

const EdgeParams* RoutingGraph::findEdge(unsigned from, unsigned to) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Exact (from, to) lookup only. Edges are directed, so a reverse match
    // would report a route that does not exist (e.g. a bus -> source query
    // echoing an unrelated source -> bus edge) and mislead control-side
    // presence checks. Snapshot publishing iterates the map directly and is
    // unaffected; this keeps the read-only query direction-honest.
    auto it = edgeMap_.find(key(from, to));
    return it == edgeMap_.end() ? nullptr : &it->second;
}

bool RoutingGraph::pathExists(unsigned from, unsigned to) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::vector<unsigned>> adj(kBusIdBase + buses_.size());
    for (const auto& entry : edgeMap_) {
        unsigned a = static_cast<unsigned>(entry.first >> 32);
        unsigned b = static_cast<unsigned>(entry.first & 0xffffffffu);
        if (a >= adj.size()) adj.resize(a + 1);
        adj[a].push_back(b);
    }
    return reachable(from, to, adj);
}

bool RoutingGraph::setBusMuted(unsigned bus, bool muted) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!isBusNode(bus) || (bus - kBusIdBase) >= buses_.size()) return false;
    buses_[bus - kBusIdBase].muted = muted;
    return true;
}

bool RoutingGraph::busMuted(unsigned bus) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!isBusNode(bus) || (bus - kBusIdBase) >= buses_.size()) return false;
    return buses_[bus - kBusIdBase].muted;
}

void RoutingGraph::addDefaultEdges(unsigned /*remoteCount*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const BusInfo& bus : buses_) {
        for (const SourceInfo& src : sources_) {
            // Mix-minus: a remote return bus never carries that remote's
            // own input. Other remotes and local sources pass normally.
            if (bus.kind == BusKind::Send &&
                src.kind == SourceKind::Remote &&
                bus.remoteIndex >= 0 &&
                bus.remoteIndex == src.remoteIndex) {
                continue;
            }
            edgeMap_.emplace(key(src.id, bus.id), EdgeParams{});
        }
    }
}

ConfigPtr RoutingGraph::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto cfg = std::make_shared<EffectiveConfig>();
    cfg->generation = ++generation_;
    cfg->sources = sources_;
    cfg->buses = buses_;
    cfg->srcEdges.reserve(edgeMap_.size());
    cfg->busEdges.reserve(buses_.size());
    for (const auto& [k, p] : edgeMap_) {
        unsigned from = static_cast<unsigned>(k >> 32);
        unsigned to = static_cast<unsigned>(k & 0xffffffffu);
        if (!isBusNode(from)) {
            SrcEdge e;
            e.sourceId = from;
            e.busId = to;
            e.gainLinear = dbToLinear(p.gainDb);
            e.muted = p.muted;
            for (int i = 0; i < kMaxChannels; ++i)
                e.channelMap[i] = p.channelMap[i];
            cfg->srcEdges.push_back(e);
        } else {
            BusEdge e;
            e.fromBus = from;
            e.toBus = to;
            e.gainLinear = dbToLinear(p.gainDb);
            e.muted = p.muted;
            for (int i = 0; i < kMaxChannels; ++i)
                e.channelMap[i] = p.channelMap[i];
            cfg->busEdges.push_back(e);
        }
    }
    // Bus edges must be evaluated in topological order: a bus's output can
    // only be forwarded after the bus itself has been mixed this block.
    // Cycles are rejected up front, so Kahn's algorithm always succeeds.
    std::unordered_map<unsigned, unsigned> busPos;
    for (unsigned i = 0; i < static_cast<unsigned>(buses_.size()); ++i)
        busPos[buses_[i].id] = i;
    const size_t nb = buses_.size();
    std::vector<std::vector<unsigned>> busAdj(nb);
    std::vector<unsigned> indegree(nb, 0);
    for (const BusEdge& e : cfg->busEdges) {
        unsigned a = busPos[e.fromBus];
        unsigned b = busPos[e.toBus];
        busAdj[a].push_back(b);
        ++indegree[b];
    }
    std::queue<unsigned> q;
    for (unsigned i = 0; i < nb; ++i)
        if (indegree[i] == 0) q.push(i);
    std::vector<BusEdge> ordered;
    ordered.reserve(cfg->busEdges.size());
    while (!q.empty()) {
        unsigned u = q.front();
        q.pop();
        for (unsigned v : busAdj[u])
            if (--indegree[v] == 0) q.push(v);
        for (const BusEdge& e : cfg->busEdges)
            if (busPos[e.fromBus] == u) ordered.push_back(e);
    }
    if (ordered.size() == cfg->busEdges.size())
        cfg->busEdges = std::move(ordered);
    return cfg;
}

std::vector<RoutingGraph::EdgeRecord> RoutingGraph::edges() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<EdgeRecord> out;
    out.reserve(edgeMap_.size());
    for (const auto& [k, p] : edgeMap_) {
        out.push_back({static_cast<unsigned>(k >> 32),
                       static_cast<unsigned>(k & 0xffffffffu), p});
    }
    return out;
}

}  // namespace mix
