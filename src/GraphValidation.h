#pragma once

#include "GraphModel.h"

// Empty means valid; otherwise returns the first diagnostic. Structure includes
// every stored edge, even on disabled/unavailable/unreachable nodes. A declared
// but disconnected required input is structurally valid during editing.
std::string graphStructureError(GraphView graph);

// Checks required connections only in the requested dependency closure. This
// is a topology check, not value evaluation or availability/bypass policy.
std::string requestedGraphError(GraphView graph, const OutputRef &requested);
