#pragma once

#include "GraphValidation.h"

namespace depth_test {
inline OutputRef image(const NodeId &id) { return {id, ports::image}; }
inline NodeId id(size_t index) { return NodeId{"depth-" + std::to_string(index)}; }

// Construct topology directly: command-by-command fixture construction would
// validate each prefix and could never supply a deliberately unsafe graph.
struct Chain {
  std::vector<Node> nodes;
  NodeId source = id(0);
  OutputRef sourceOutput = image(source);
  OutputRef output = image(source);
  explicit Chain(size_t depth, NodeOperation operation = NodeOperation::TestImage) {
    nodes.reserve(depth);
    Node root(NodeOperation::Source); root.id = source;
    nodes.push_back(std::move(root));
    for (size_t i = 1; i < depth; ++i) {
      Node node(operation); node.id = id(i);
      node.fixedInputs.at(ports::image) = output;
      output = image(node.id);
      nodes.push_back(std::move(node));
    }
  }
  GraphView view() const {
    if (output.outputPortId != ports::image) return {nodes, source, sourceOutput};
    return {nodes, source, output};
  }
};

// Independent schema fixtures bypass the production writer's validation. Depth
// counts nodes, including Source; fragment depth counts only fragment members.
inline std::string nodesJson(size_t depth, bool fragment = false) {
  std::string nodes = fragment ? "" : R"({"id":"depth-0","kind":"source"})";
  for (size_t i = 1; i < depth + (fragment ? 1 : 0); ++i) {
    if (!nodes.empty()) nodes += ',';
    nodes += "{\"id\":\"depth-" + std::to_string(i) +
        "\",\"kind\":\"image-processor\",\"backend\":\"native\","
        "\"identifier\":\"rawnode.native.exposure\",\"enabled\":true,\"params\":{\"exposure\":0},"
        "\"inputs\":{\"image\":";
    nodes += fragment && i == 1 ? "null" :
        "{\"node\":\"depth-" + std::to_string(i - 1) + "\",\"port\":\"image\"}";
    nodes += "}}";
  }
  return nodes;
}
inline std::string gradeJson(size_t depth) {
  return "{\"color\":{\"rawWorking\":{\"colorSpace\":\"rec2020\",\"gamma\":\"linear\"},"
      "\"output\":{\"colorSpace\":\"display-p3\",\"gamma\":\"srgb\"}},"
      "\"graph\":{\"primarySource\":\"depth-0\",\"documentOutput\":{\"node\":\"depth-" +
      std::to_string(depth - 1) + "\",\"port\":\"image\"},\"nodes\":[" + nodesJson(depth) + "]},"
      "\"positions\":{\"depth-1\":{\"x\":42,\"y\":-8}}}";
}
inline std::string fileJson(size_t depth) {
  return "{\"format\":\"rawnode\",\"version\":1,\"grade\":" + gradeJson(depth) +
      ",\"metadata\":{\"mark\":\"pick\"}}";
}
inline bool depthError(const std::string &error) {
  return error.find("Graph dependency depth exceeds supported limit of " +
      std::to_string(kMaxGraphDependencyDepth) + " nodes at ") != std::string::npos;
}
} // namespace depth_test
