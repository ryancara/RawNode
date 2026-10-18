#pragma once

#include "GraphModel.h"

// Longest stored dependency chain, counting both endpoints (a leaf is depth 1).
// Bounds recursive readiness/evaluation after iterative structural validation.
inline constexpr size_t kMaxGraphDependencyDepth = 256;

// Empty means valid; otherwise returns the first diagnostic. Structure includes
// every stored edge, even on disabled/unavailable/unreachable nodes. A declared
// but disconnected required input is structurally valid during editing.
std::string graphStructureError(GraphView graph);

// Checks required connections only in the requested dependency closure. This
// is a topology check, not value evaluation or availability/bypass policy.
std::string requestedGraphError(GraphView graph, const OutputRef &requested);
