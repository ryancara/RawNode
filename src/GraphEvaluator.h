#pragma once

#include "GraphModel.h"
#include "Mask.h"

#include <utility>
#include <variant>

using ImageValue = std::shared_ptr<const Image>;
using MaskValue = std::shared_ptr<const Mask>;
using GraphValue = std::variant<std::monostate, ImageValue, MaskValue>;

enum class EvaluationPurpose { Preview, Export, Inspection };
struct EvaluationContext {
  EvaluationPurpose purpose = EvaluationPurpose::Preview;
  int fullSourceW = 0, fullSourceH = 0;
  int evalSourceW = 0, evalSourceH = 0;
};

enum class EvaluationState { Available, Inactive, Unavailable, Error, Cancelled };
enum class DiagnosticLevel { Warning, Error };
struct EvaluationDiagnostic {
  DiagnosticLevel level;
  OutputRef endpoint;
  std::string message;
};

// One actual coverage use, anchored in the consuming Image operation's OUTPUT
// raster, where mixing samples Mask. Reference controls generation, not placement.
// Multiple uses of one Mask stay separate: callers must consider every record
// (and Unknown mappings), never select an arbitrary consumer for future handles.
struct MaskPlacement {
  OutputRef mask;
  InputAddress consumer;
  CoordinateMapping maskToImage = CoordinateMapping::identity();
};

// Available results own an immutable value; all other states have no raster.
// Inactive is Mask-only and is created by evaluator bypass/propagation, never
// by processor execution. Optional coverage treats it as absent without warning.
// Type is absent when structure/endpoint validation cannot resolve a request. Runtime
// callers can pair this complete result with their publication/request identity.
struct EvaluationResult {
  OutputRef requested;
  std::optional<ValueType> valueType;
  EvaluationState state = EvaluationState::Error;
  GraphValue value;
  std::vector<EvaluationDiagnostic> diagnostics;
  // Image-only transient metadata for exactly this evaluated value. It travels
  // with the existing result/publication generation, never with saved rasters.
  std::vector<MaskPlacement> maskPlacements;
};

namespace graph_test {

struct SlotValues {
  SlotId id;
  ImageValue image;
  MaskValue mask;  // Absent/inactive coverage is null; unavailable also warns.
};
struct InputValues {
  ImageValue image;
  MaskValue mask;
  ImageValue reference;
  std::vector<SlotValues> slots;  // Semantic vector order, never sorted by ID.
};
struct OperationResult {
  EvaluationState state = EvaluationState::Available;
  std::map<PortId, GraphValue> outputs;  // A multi-output operation may compute a bundle once.
  std::string message;
};
// Narrow execution seam for static Test* interfaces. Dependency policy,
// bypass, availability and raster validation remain in the real evaluator;
// toy pixel arithmetic and mutable counters live only in tests.
class Executor {
 public:
  virtual ~Executor() = default;
  virtual OperationResult execute(const Node &node, const EvaluationContext &context,
                                  const InputValues &inputs, const RenderCancellation &cancellation) = 0;
};

}  // namespace graph_test

// The caller owns graph/processor execution exclusion for the whole call. No
// App, worker, persistent cache or scheduling state is required. Source pixels
// are shared, not copied for Source/bypass/missing-processor pass-through.
EvaluationResult evaluateGraph(GraphView graph, const OutputRef &requested,
                               const EvaluationContext &context, ImageValue source,
                               const RenderCancellation &cancellation = {},
                               graph_test::Executor *testExecutor = nullptr);

// Bounded paired preview: one evaluation memo/plan, two retained endpoint values.
// The caller publishes the pair under a single request/generation identity.
std::pair<EvaluationResult, EvaluationResult> evaluateGraphPair(
    GraphView graph, const OutputRef &image, const OutputRef &mask,
    const EvaluationContext &context, ImageValue source,
    const RenderCancellation &cancellation = {}, graph_test::Executor *testExecutor = nullptr);
