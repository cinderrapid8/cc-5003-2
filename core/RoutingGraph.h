#pragma once

#include "EffectiveConfig.h"
#include "Types.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mix {

// Control-side routing model. All mutators are serialized by an internal
// mutex so GUI threads can edit while a snapshot is being published.
// Nodes are sources and buses; edges run source -> bus or bus -> bus.
class RoutingGraph {
public:
    unsigned addSource(SourceKind kind, std::string name, int channels,
                       int remoteIndex = -1);
    unsigned addBus(BusKind kind, std::string name, int channels,
                    int remoteIndex = -1);

    // Add an edge, or fail before mutating anything when the edge would
    // close a feedback loop. Self loops are just one particular cycle and
    // go through the same reachability check, as do source->source links.
    bool setEdge(unsigned from, unsigned to, EdgeParams params);
    bool updateEdge(unsigned from, unsigned to, EdgeParams params);
    bool removeEdge(unsigned from, unsigned to);

    const EdgeParams* findEdge(unsigned from, unsigned to) const;
    bool createsCycle(unsigned from, unsigned to) const;
    bool pathExists(unsigned from, unsigned to) const;

    bool setBusMuted(unsigned bus, bool muted);
    bool busMuted(unsigned bus) const;

    // Build the conference default: every source feeds every bus, except
    // each remote return bus excludes its own remote input (mix-minus),
    // so a remote caller never receives its own audio straight back.
    void addDefaultEdges(unsigned remoteCount);

    // Freeze the current graph into an immutable config for the RT thread.
    ConfigPtr snapshot();

    const std::vector<SourceInfo>& sources() const { return sources_; }
    const std::vector<BusInfo>& buses() const { return buses_; }

    struct EdgeRecord {
        unsigned from;
        unsigned to;
        EdgeParams params;
    };
    std::vector<EdgeRecord> edges() const;

private:
    struct Node {
        bool isBus;
        unsigned index;  // index into sources_ or buses_
    };

    bool isBusNode(unsigned id) const;
    bool nodeExists(unsigned id) const;
    bool reachable(unsigned from, unsigned to,
                   const std::vector<std::vector<unsigned>>& adj) const;

    mutable std::mutex mutex_;
    std::vector<SourceInfo> sources_;
    std::vector<BusInfo> buses_;
    std::unordered_map<unsigned long long, EdgeParams> edgeMap_;
    uint64_t generation_ = 0;

    static unsigned long long key(unsigned from, unsigned to) {
        return (static_cast<unsigned long long>(from) << 32) | to;
    }
};

}  // namespace mix
