#include "GraphEvaluator.h"

#include "GraphValidation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace {

struct OutputLess {
  bool operator()(const OutputRef &a, const OutputRef &b) const {
    return std::tie(a.nodeId.value, a.outputPortId.value) < std::tie(b.nodeId.value, b.outputPortId.value);
  }
};
struct Outcome {
  EvaluationState state = EvaluationState::Available;
  GraphValue value;
  bool validated = false;
};
static bool stopped(const Outcome &outcome) { return outcome.state != EvaluationState::Available; }
static bool rasterShape(int w, int h, size_t channels, size_t samples) {
  if (w <= 0 || h <= 0) return false;
  const size_t width = static_cast<size_t>(w), height = static_cast<size_t>(h);
  if (width > std::numeric_limits<size_t>::max() / height / channels) return false;
  return width * height * channels == samples;
}

// One call owns memo identity and remaining uses. A consumed raster leaves the
// memo at its last use; the consumer's local shared value owns it through that
// operation. Completed metadata stays memoized so work/diagnostics never repeat.
class Evaluation {
 public:
  Evaluation(GraphView graph, const EvaluationContext &context, ImageValue source,
             const RenderCancellation &cancellation, graph_test::Executor *testExecutor)
      : graph_(graph), context_(context), source_(std::move(source)), cancellation_(cancellation), executor_(testExecutor) {}

  EvaluationResult run(const OutputRef &requested) {
    EvaluationResult result{requested, std::nullopt, EvaluationState::Error, {}, {}};
    if (cancellation_.cancelled()) {
      result.state = EvaluationState::Cancelled;
      return result;
    }
    // Required-input readiness is deliberately NOT a preflight gate: bypass
    // may avoid a connected branch with missing required inputs. All stored
    // cycles/types/endpoints still have to be structurally valid. The iterative
    // validator also bounds plan()/visit() recursion by kMaxGraphDependencyDepth.
    // Absent Reference adds only the input-free primary Source; bundles and
    // bypass never introduce a recursive dependency outside those bounds.
    const auto structure = graphStructureError(graph_);
    if (!structure.empty()) return finish(result, failure(requested, structure));
    const auto *port = findOutput(graph_, requested);
    if (!port) return finish(result, failure(requested, "Requested output does not exist"));
    result.valueType = port->type;
    if (context_.fullSourceW <= 0 || context_.fullSourceH <= 0 || context_.evalSourceW <= 0 || context_.evalSourceH <= 0)
      return finish(result, failure(requested, "Invalid evaluation Source dimensions"));
    if (!source_ || !rasterShape(source_->w, source_->h, 4, source_->px.size()))
      return finish(result, failure({graph_.primarySource, ports::image}, "Invalid Source Image raster"));
    if (source_->w != context_.evalSourceW || source_->h != context_.evalSourceH)
      return finish(result, failure(requested, "Evaluation context does not match Source raster dimensions"));
    requested_ = requested;
    plan(requested);
    return finish(result, visit(requested));
  }

 private:
  EvaluationResult finish(EvaluationResult result, Outcome outcome) {
    result.state = outcome.state;
    result.value = std::move(outcome.value);
    if (result.state != EvaluationState::Cancelled) result.diagnostics = std::move(diagnostics_);
    return result;
  }
  void plan(const OutputRef &output) {
    const auto planned = planned_.emplace(output, 1.0);
    if (!planned.second) return;
    const Node &node = *findNode(graph_, output.nodeId);
    // Enabled bundle execution consumes its inputs once, even when several
    // distinct outputs are requested. Bypass executes each pass-through output.
    if (node.operation == NodeOperation::TestMultipleOutputs && node.enabled &&
        !plannedBundles_.insert(node.id.value).second) return;
    const double strength = node.operation == NodeOperation::ImageProcessor ? double(node.strength) : 1.0;
    planned.first->second = strength;
    const auto dependency = [&](const OutputRef &producer) {
      ++remaining_[producer];
      plan(producer);
    };
    const auto connected = [&](const PortId &port, std::optional<SlotId> slot = std::nullopt) {
      const auto *binding = findInput(graph_, {node.id, slot, port});
      if (binding && *binding) dependency(**binding);
    };
    const auto *port = findOutput(graph_, output);
    if (!node.enabled || (node.operation == NodeOperation::ImageProcessor && strength == 0)) {
      if (port->bypassInput) {
        connected(*port->bypassInput);
        return;
      }
      if (port->type == ValueType::Mask) return;
    }
    switch (node.operation) {
      case NodeOperation::ImageProcessor:
        connected(ports::image);
        if (node.processor) connected(ports::mask);
        break;
      case NodeOperation::TestImage:
      case NodeOperation::TestImageToMask:
      case NodeOperation::TestMultipleOutputs:
        connected(ports::image);
        break;
      case NodeOperation::TestMaskedImage:
        connected(ports::image);
        if (node.enabled) connected(ports::mask);
        break;
      case NodeOperation::TestMaskProcessor:
        connected(ports::mask);
        break;
      case NodeOperation::TestReferenceMask: {
        const auto *reference = findInput(graph_, {node.id, std::nullopt, ports::reference});
        if (*reference) dependency(**reference);
        else dependency({graph_.primarySource, ports::image});
        break;
      }
      case NodeOperation::TestSlotted:
        if (node.enabled) connected(ports::reference);
        for (const auto &slot : node.slots) {
          if (!slot.settings.enabled) continue;
          connected(ports::image, slot.id);
          if (!node.enabled) break;
          connected(ports::mask, slot.id);
        }
        break;
      case NodeOperation::Source:
      case NodeOperation::TestUnavailableImage:
      case NodeOperation::TestUnavailableMask:
        break;
    }
  }
  void releaseUnused(const OutputRef &output) {
    if (output == *requested_) return;
    const auto uses = remaining_.find(output);
    if (uses != remaining_.end() && uses->second != 0) return;
    const auto cached = memo_.find(output);
    if (cached != memo_.end()) cached->second.value = std::monostate{};
  }
  Outcome dependency(const OutputRef &output) {
    auto result = visit(output);
    auto uses = remaining_.find(output);
    if (uses != remaining_.end() && uses->second != 0) --uses->second;
    releaseUnused(output);
    return result;
  }
  void diagnose(DiagnosticLevel level, const OutputRef &endpoint, std::string message) {
    diagnostics_.push_back({level, endpoint, std::move(message)});
  }
  Outcome failure(const OutputRef &endpoint, std::string message) {
    if (cancellation_.cancelled()) return {EvaluationState::Cancelled, {}};
    diagnose(DiagnosticLevel::Error, endpoint, std::move(message));
    return {EvaluationState::Error, {}};
  }

  Outcome visit(const OutputRef &output) {
    if (cancellation_.cancelled()) return {EvaluationState::Cancelled, {}};
    auto cached = memo_.find(output);
    if (cached != memo_.end()) return validate(output, cached->second);
    if (!visiting_.insert(output).second) return failure(output, "Cycle encountered during evaluation");
    Outcome result;
    try {
      const Node *node = findNode(graph_, output.nodeId);
      if (!node || !findOutput(graph_, output)) result = failure(output, "Output endpoint disappeared during evaluation");
      else result = executeNode(*node, output);
    } catch (const std::exception &error) {
      result = failure(output, std::string("Graph operation failed: ") + error.what());
    } catch (...) {
      result = failure(output, "Graph operation failed with an unknown exception");
    }
    visiting_.erase(output);
    if (cancellation_.cancelled()) result = {EvaluationState::Cancelled, {}};
    auto stored = memo_.emplace(output, std::move(result));
    return validate(output, stored.first->second);
  }

  Outcome input(const OutputRef &consumer, const PortId &port, std::optional<SlotId> slot = std::nullopt) {
    const InputAddress address{consumer.nodeId, slot, port};
    const auto *declaration = findInputDeclaration(graph_, address);
    const auto *binding = findInput(graph_, address);
    if (!declaration || !binding) return failure(consumer, "Input storage does not exist");
    if (*binding) return dependency(**binding);
    if (declaration->required) {
      const std::string location = slot ? "[Slot " + std::to_string(slot->value) + "]" : "";
      return failure(consumer, "Missing required input: " + consumer.nodeId.value + location + "." + port.value);
    }
    return {}; // A genuinely absent optional input, not an unavailable value.
  }

  Outcome coverage(const OutputRef &consumer, ImageValue image, std::optional<SlotId> slot = std::nullopt) {
    auto value = input(consumer, ports::mask, slot);
    if (value.state == EvaluationState::Inactive) return {};
    if (value.state == EvaluationState::Unavailable) {
      diagnose(DiagnosticLevel::Warning, consumer, "Optional Mask unavailable; using full coverage" +
               (slot ? " for Slot " + std::to_string(slot->value) : std::string()));
      return {}; // Fail-open occurs only at this optional coverage use.
    }
    if (stopped(value) || std::holds_alternative<std::monostate>(value.value)) return value;
    const auto mask = std::get<MaskValue>(value.value);
    if (mask->w != image->w || mask->h != image->h)
      return failure(consumer, "Image/Mask dimension mismatch: Image " + std::to_string(image->w) + "x" +
                     std::to_string(image->h) + ", Mask " + std::to_string(mask->w) + "x" + std::to_string(mask->h));
    return value;
  }

  Outcome executeNode(const Node &node, const OutputRef &output) {
    if (node.operation == NodeOperation::Source) return {EvaluationState::Available, source_};
    const auto *port = findOutput(graph_, output);
    const double strength = planned_.at(output);
    if (!node.enabled || (node.operation == NodeOperation::ImageProcessor && strength == 0)) {
      if (port->bypassInput) return input(output, *port->bypassInput);
      if (port->type == ValueType::Mask) return {EvaluationState::Inactive, {}};
    }
    graph_test::InputValues inputs;
    switch (node.operation) {
      case NodeOperation::ImageProcessor:
      case NodeOperation::TestImage:
      case NodeOperation::TestImageToMask:
      case NodeOperation::TestMultipleOutputs:
      case NodeOperation::TestMaskedImage: {
        auto upstream = input(output, ports::image);
        if (stopped(upstream)) return upstream;
        inputs.image = std::get<ImageValue>(upstream.value);
        if (node.operation == NodeOperation::ImageProcessor) return maskedProcessor(node, output, inputs.image, strength);
        if (node.operation == NodeOperation::TestMaskedImage) {
          auto mask = coverage(output, inputs.image);
          if (stopped(mask)) return mask;
          if (const auto *value = std::get_if<MaskValue>(&mask.value)) inputs.mask = *value;
        }
        break;
      }
      case NodeOperation::TestMaskProcessor: {
        auto upstream = input(output, ports::mask);
        if (stopped(upstream)) return upstream;
        inputs.mask = std::get<MaskValue>(upstream.value);
        break;
      }
      case NodeOperation::TestReferenceMask: {
        auto frame = input(output, ports::reference);
        if (stopped(frame)) return frame; // Connected unavailable Reference never defaults to Source.
        if (std::holds_alternative<std::monostate>(frame.value)) frame = dependency({graph_.primarySource, ports::image});
        if (stopped(frame)) return frame;
        inputs.reference = std::get<ImageValue>(frame.value);
        break;
      }
      case NodeOperation::TestSlotted: {
        // This test join's bypass base is its first enabled Slot Image. Neither
        // later Slots nor their coverage/Reference branches are needed in bypass.
        if (!node.enabled) {
          for (const auto &slot : node.slots)
            if (slot.settings.enabled) return input(output, ports::image, slot.id);
          return failure(output, "Bypassed test join has no base Slot");
        }
        auto frame = input(output, ports::reference);
        if (stopped(frame)) return frame;
        if (const auto *value = std::get_if<ImageValue>(&frame.value)) inputs.reference = *value;
        for (const auto &slot : node.slots) {
          if (!slot.settings.enabled) continue;
          auto layer = input(output, ports::image, slot.id);
          if (stopped(layer)) return layer;
          auto image = std::get<ImageValue>(layer.value);
          auto mask = coverage(output, image, slot.id);
          if (stopped(mask)) return mask;
          MaskValue maskValue;
          if (const auto *value = std::get_if<MaskValue>(&mask.value)) maskValue = *value;
          inputs.slots.push_back({slot.id, std::move(image), std::move(maskValue)});
        }
        break;
      }
      case NodeOperation::TestUnavailableImage:
      case NodeOperation::TestUnavailableMask:
        break;
      case NodeOperation::Source:
        break;
    }
    return executeTest(node, output, inputs);
  }

  Outcome maskedProcessor(const Node &node, const OutputRef &output, ImageValue image, double strength) {
    // Missing unary effects retain their defined pass-through fallback, with a
    // warning. Their unused coverage must not turn that fallback into an edit.
    if (!node.processor) return processor(node, output, std::move(image));
    auto mask = coverage(output, image);
    if (stopped(mask)) return mask;
    auto processed = processor(node, output, image);
    processed = validate(output, processed);
    if (stopped(processed)) return processed;
    const auto value = std::get<ImageValue>(processed.value);
    const auto *coverageMask = std::get_if<MaskValue>(&mask.value);
    if (strength == 1 && !coverageMask) return processed;
    if (value->w != image->w || value->h != image->h)
      return failure(output, "Image input/processor output dimension mismatch for Mask/Strength mixing");
    auto mixed = std::make_shared<Image>(*image);
    for (size_t pixel = 0; pixel < image->px.size() / 4; ++pixel) {
      const double amount = strength * (coverageMask ? (*coverageMask)->px[pixel] : 1.0);
      // Exact endpoints also avoid 0*Inf and preserve unchanged alpha samples.
      if (amount == 0) continue;
      for (size_t channel = 0; channel < 4; ++channel) {
        const size_t sample = pixel * 4 + channel;
        if (amount == 1) mixed->px[sample] = value->px[sample];
        else if (image->px[sample] != value->px[sample])
          mixed->px[sample] = float(double(image->px[sample]) +
              (double(value->px[sample]) - image->px[sample]) * amount);
      }
    }
    return {EvaluationState::Available, ImageValue(std::move(mixed))};
  }

  Outcome processor(const Node &node, const OutputRef &output, ImageValue image) {
    if (!node.processor) {
      diagnose(DiagnosticLevel::Warning, output, "Missing Image processor; passing input through" +
               (node.storedIdentifier.empty() ? std::string() : ": " + node.storedIdentifier));
      return {EvaluationState::Available, std::move(image)};
    }
    if (cancellation_.cancelled()) return {EvaluationState::Cancelled, {}};
    node.processor->setRenderSize(image->w, image->h);
    if (cancellation_.cancelled()) return {EvaluationState::Cancelled, {}};
    auto rendered = std::make_shared<Image>();
    const auto result = node.processor->render(*image, *rendered, cancellation_);
    if (cancellation_.cancelled()) return {EvaluationState::Cancelled, {}};
    if (!result.ok)
      return failure(output, "Processor render failed (" + std::to_string(result.backendCode) + "): " + result.message);
    return {EvaluationState::Available, ImageValue(std::move(rendered))};
  }

  Outcome executeTest(const Node &node, const OutputRef &output, const graph_test::InputValues &inputs) {
    if (!executor_) return failure(output, "Test graph operation has no executor");
    if (cancellation_.cancelled()) return {EvaluationState::Cancelled, {}};
    auto result = executor_->execute(node, context_, inputs, cancellation_);
    if (cancellation_.cancelled()) return {EvaluationState::Cancelled, {}};
    if (result.state == EvaluationState::Inactive)
      return failure(output, "Executor must not return Inactive; only evaluator Mask bypass/propagation may do so");
    if (result.state == EvaluationState::Error)
      return failure(output, result.message.empty() ? "Test graph operation failed" : result.message);
    if (result.state == EvaluationState::Cancelled) return {EvaluationState::Cancelled, {}};
    if (result.state == EvaluationState::Unavailable) {
      diagnose(DiagnosticLevel::Warning, output, result.message.empty() ? "Test output unavailable" : result.message);
      for (const auto &port : nodeInterface(node.operation).fixedPorts)
        if (port.direction == PortDirection::Output && port.id != output.outputPortId)
          memo_.emplace(OutputRef{node.id, port.id}, Outcome{EvaluationState::Unavailable, {}});
      return {EvaluationState::Unavailable, {}};
    }
    if (!result.message.empty()) diagnose(DiagnosticLevel::Warning, output, result.message);
    for (const auto &entry : result.outputs) {
      if (!nodeInterface(node.operation).findFixedPort(entry.first, PortDirection::Output))
        return failure(output, "Test operation returned an undeclared output");
    }
    for (const auto &port : nodeInterface(node.operation).fixedPorts)
      if (port.direction == PortDirection::Output && result.outputs.count(port.id) != 1)
        return failure(output, "Test operation did not produce all declared outputs");
    const auto requested = result.outputs.find(output.outputPortId);
    if (requested == result.outputs.end()) return failure(output, "Test operation did not produce the requested output");
    // Public and memo identity remain OutputRef even when one execution naturally
    // produces several outputs. Secondary values are checked when requested.
    for (auto &entry : result.outputs)
      if (entry.first != output.outputPortId) {
        const OutputRef secondary{node.id, entry.first};
        memo_.emplace(secondary, Outcome{EvaluationState::Available, std::move(entry.second)});
        releaseUnused(secondary);
      }
    return {EvaluationState::Available, std::move(requested->second)};
  }

  Outcome validate(const OutputRef &output, Outcome &result) {
    const auto *port = findOutput(graph_, output);
    if (!port) return result = failure(output, "Output endpoint does not exist");
    if (result.state == EvaluationState::Inactive && port->type != ValueType::Mask)
      return result = failure(output, "Image output must not be Inactive");
    if (stopped(result) || result.validated) return result;
    if (port->type == ValueType::Image) {
      const auto *image = std::get_if<ImageValue>(&result.value);
      if (!image || !*image || !rasterShape((*image)->w, (*image)->h, 4, (*image)->px.size()))
        return result = failure(output, "Invalid Image output raster/type");
    } else {
      const auto *mask = std::get_if<MaskValue>(&result.value);
      if (!mask || !*mask || !rasterShape((*mask)->w, (*mask)->h, 1, (*mask)->px.size()))
        return result = failure(output, "Invalid Mask output raster/type");
      bool clamp = false;
      for (float sample : (*mask)->px) {
        if (!std::isfinite(sample)) return result = failure(output, "Non-finite Mask coverage");
        if (sample < 0.0f || sample > 1.0f) clamp = true;
      }
      if (clamp) {
        auto bounded = std::make_shared<Mask>(**mask);
        for (float &sample : bounded->px) sample = std::clamp(sample, 0.0f, 1.0f);
        result.value = MaskValue(std::move(bounded));
      }
    }
    result.validated = true;
    return result;
  }

  GraphView graph_;
  EvaluationContext context_;
  ImageValue source_;
  RenderCancellation cancellation_;
  graph_test::Executor *executor_;
  std::map<OutputRef, Outcome, OutputLess> memo_;
  std::optional<OutputRef> requested_;
  std::map<OutputRef, size_t, OutputLess> remaining_;
  std::map<OutputRef, double, OutputLess> planned_; // Strength sampled once, shared by bypass and mixing.
  std::set<std::string> plannedBundles_;
  std::set<OutputRef, OutputLess> visiting_;
  std::vector<EvaluationDiagnostic> diagnostics_;
};

}  // namespace

EvaluationResult evaluateGraph(GraphView graph, const OutputRef &requested,
                               const EvaluationContext &context, ImageValue source,
                               const RenderCancellation &cancellation, graph_test::Executor *testExecutor) {
  return Evaluation(graph, context, std::move(source), cancellation, testExecutor).run(requested);
}
