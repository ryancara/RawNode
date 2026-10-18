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
or node count: 256 nodes (255 stored edges) are supported, 257 are rejected.
The validator will use iterative DFS over authoritative fixed/Slot bindings,
including Image, Mask, Reference, disabled, unavailable and disconnected topology.
Completed nodes retain their longest upstream depth, so already visited shared
dependencies cannot hide a longer path. Lookup and marks are operation-local.
No processor executes for a rejected graph, and no partial grade is published.

Requested-input checking retains recursion only behind successful structural
validation. Evaluator planning and execution remain sequential and recursive.
Every recursive dependency follows a stored edge, except an absent generator
Reference defaults directly to the input-free primary Source. Thus there can be
at most 257 nested node visits, with no extra recursion for output bundles or
sequential Slot siblings. Bypass selects a subset of stored edges. The limit
leaves room for hundreds of photo-editing operations without requiring a new
execution model. On Linux/GCC 14, real boundary evaluation succeeds on an
explicit 512 KiB pthread stack, including alternating optional Mask coverage and
Reference dependencies with an implicit Source leaf. This exercises the extra
`coverage()`/`input()` nesting, not just unary Image chains. Compiler stack-usage
output and sanitizer runs supplement this empirical bound; it is not a promise
for arbitrary thread stack sizes or third-party processor stack requirements.
macOS validation remains pending.

The DFS checks all bindings for endpoint/type errors first, then walks every
stored node. Visiting marks detect cycles; completed marks retain longest depth.
An active chain stops before allocating more than 256 explicit frames. A long
cycle may therefore return the depth diagnostic before its back edge is reached;
ordinary cycles, including a 255-node cycle, retain `Graph contains a cycle`.
No adjacency list or persistent index is introduced. Structural traversal and
lookup take expected O(nodes + bindings) work, apart from existing SlotId checks.

Tests must cover the exact boundary with real Image/Mask/Slot evaluation, shallow
and deep cycles, completed shared ancestors, disconnected and disabled branches,
wide shallow topology, actual decoding/restoration, protected photo departure,
preset/clipboard atomicity and command refusal without renderer demand changes.
Expected unchanged pixels, endpoint identity, original sidecar bytes and unchanged
document snapshots are independent behavioural oracles.
