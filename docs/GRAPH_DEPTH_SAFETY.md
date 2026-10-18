# Graph traversal depth safety

## Reachable paths and design

The traversal audit before implementation found these production paths:

- `graph::StagedEdit` validates the original topology and staged commit with
  `graphStructureError()` before acquiring `DocumentMutation`. Direct document
  commands and App commands share this gate.
- `graphRecordError()` constructs backend-free Nodes and calls the same validator.
  Graph-file decoding, encoding, transfer decoding and encoding all use it.
  Fragment validation adds a disconnected synthetic Source and checks every
  fragment node. Decoder destinations publish only after full validation.
- `restoreGraph()` validates records before constructing processors and validates
  restored topology again. It also protects programmatically supplied grades.
- Photo opening decodes and restores before publishing. Failure opens a protected
  Source view with a persistent warning; it never performs the initial grade save
  or overwrites the saved sidecar. Departure retains that protection.
- Full-grade preset/clipboard Replace uses the codec and `restoreGraph()` before
  publication. Codec/colour refusal precedes `DocumentMutation`; processor
  construction still holds that ownership. Append/Paste validates the incoming
  transfer and runs `insertImageChain()` on backend-free local staging to check
  the combined topology before taking ownership for processor construction.
  The actual commit uses the same command, preserving one attachment policy.
- `requestedGraphError()` calls structural validation before recursively checking
  required inputs in only the requested closure. It is not an evaluator preflight:
  evaluation may bypass incomplete dependencies.
- Every `evaluateGraph()` call, including direct pixel tests, RenderPipeline and
  RenderRuntime preview/inspection/export, enters `Evaluation::run()` and passes
  structural validation before recursive planning or execution.

Acceptance is based on longest dependency chains, independently of storage order
or total graph node count: 128 nodes (127 stored edges) are supported, 129 are rejected.
The validator uses iterative DFS over authoritative fixed/Slot bindings,
including Image, Mask, Reference, disabled, unavailable and disconnected topology.
Completed nodes retain their longest upstream depth, so already visited shared
dependencies cannot hide a longer path. Lookup and marks are operation-local.
No processor executes for a rejected graph, and no partial grade is published.
Stored/imported grades use the same limit and existing refusal/protected-sidecar
policy, without rewriting or truncating unsupported saved data.

Requested-input checking retains recursion only behind successful structural
validation. Evaluator planning and execution remain sequential and recursive.
Every recursive dependency follows a stored edge, except an absent generator
Reference defaults directly to the input-free primary Source. Thus there can be
at most 129 nested node visits, with no extra recursion for output bundles or
sequential Slot siblings. Bypass selects a subset of stored edges. The limit
allows far more depth than typical photo-editing workflows need without changing
worker stack sizes or the execution model.

## Stack rationale and evidence

macOS preview/export workers use default 512 KiB stacks, smaller than the usual
multi-MiB Linux defaults. Passing an optimised Linux build on a reduced stack
therefore does not establish safety for an unoptimised macOS build.

The independent PR review reported these approximate stack measurements across
Image and Mask/Reference coverage chains at the former 256-node boundary:

| Build configuration | Observed stack use |
| --- | ---: |
| GCC Release with LTO | 334–406 KiB |
| clang Release | 286–362 KiB |
| clang Debug without optimisation | 748–840 KiB |
| ASan/UBSan | 1.96–2.15 MiB |

The reviewer reproduced an actual overflow for a valid 256-node graph with
unoptimised clang and a 512 KiB stack. At 128 nodes, the reviewer measured about
378 KiB for an Image chain and 425 KiB for a Mask/Reference coverage chain in
clang Debug. These observed measurements support the conservative 128-node
policy; they are review evidence, not measurements independently repeated by
this Linux implementation environment or guarantees for every build/graph.

Sanitizer instrumentation can substantially increase stack consumption. Neither
a normal Release pass nor an optimised sanitizer pass establishes safety for
sanitizer-instrumented macOS workers on their default 512 KiB stacks. Reduced-stack
checks and their exact configurations are reported separately in the PR.
macOS application validation remains pending after targeted independent re-review.
The limit is a practical bound, not a guarantee for arbitrary tiny stacks,
sanitizer configurations or third-party processor stack requirements.

The DFS checks all bindings for endpoint/type errors first, then walks every
stored node. Visiting marks detect cycles; completed marks retain longest depth.
An active chain stops before allocating more than 128 explicit frames. A long
cycle may therefore return the depth diagnostic before its back edge is reached;
ordinary cycles, including a 127-node cycle, retain `Graph contains a cycle`.
No adjacency list or persistent index is introduced. Structural traversal and
lookup take expected O(nodes + bindings) work, apart from existing SlotId checks.

Regression tests cover the exact boundary with real Image/Mask/Slot evaluation,
shallow and deep cycles, completed shared ancestors, disconnected and disabled branches,
wide shallow topology, actual decoding/restoration, protected photo departure,
preset/clipboard atomicity and command refusal without renderer demand changes.
Expected unchanged pixels, endpoint identity, original sidecar bytes and unchanged
document snapshots are independent behavioural oracles.

The longest-path regressions explicitly use deep-first/shallow-second inputs in
both repeated Slots and fixed Image/Mask bindings, followed by enough consumers
to reach 128 or 129 nodes. Their shared ancestor ensures completed dependencies
are included without letting later shallow inputs shorten the maximum. A
Source-first chain with only the processors reversed exercises parent-binding
revisits after child completion: active DFS stack size excludes the already
completed Source and cannot replace the longest-path calculation. Expected
boundary counts follow directly from fixture construction, not a copied DFS.
