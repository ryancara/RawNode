#pragma once

#include <atomic>

class RenderRuntime;
class RenderCancellationTestAccess;

// A borrowed, read-only token for one evaluation. The runtime outlives every
// evaluation using it. An empty token is non-cancellable (full-resolution export
// and direct evaluator tests). Processors need no knowledge of demand or epochs.
class RenderCancellation {
 public:
  RenderCancellation() = default;
  bool cancelled() const noexcept { return epoch_ && epoch_->load() != expected_; }
  bool interactive() const noexcept { return epoch_ != nullptr; }

 private:
  friend class RenderRuntime;
  // A borrowed test-owned epoch permits deterministic worker-free evaluator
  // cancellation tests without exposing runtime scheduling controls.
  friend class RenderCancellationTestAccess;
  RenderCancellation(const std::atomic<int> &epoch, int expected)
      : epoch_(&epoch), expected_(expected) {}
  const std::atomic<int> *epoch_ = nullptr;
  int expected_ = 0;
};
