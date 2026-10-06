# RawNode Architecture

**Status:** Authoritative target architecture.

This document records the architecture approved after the RawNode <-> vkdt
architecture audit and independent Claude review. Current source code may still
be transitioning toward this model.

Read this together with:

- `docs/PROJECT_CONTEXT.md` for current project state and roadmap;
- `docs/DECISIONS.md` for accepted durable design decisions;
- `docs/ARCHITECTURE_AUDIT.md` for the audit evidence and reconciliation that
  led here.

## Architectural north star

Every new RawNode feature should have an obvious home.

If adding a feature requires teaching unrelated parts of the application how
that feature works, the boundary is probably wrong.

This principle is deliberately stronger than "use abstractions". RawNode should
not accumulate feature-specific schedulers, queues, pending flags, persistence
paths, export paths or UI-to-render protocols simply because a new feature was
added.

A feature should normally fit one of the established responsibilities below.

## High-level model

The target model is:

    UI
     |
     v
    Document / Graph API
     |
     +----------------------+----------------------+
     |                                             |
     v                                             v
    Persistent document                        Render runtime
    -------------------                        --------------
    Source                                     Worker lifetime
    Graph topology                             Execution ownership
    Node parameters                            Edit/export exclusion
    Colour semantics                           Cancellation
    Sidecar state                              Preview/display demand
     |                                             |
     +----------------------+----------------------+
                            |
                            v
                     Graph evaluator
                            |
                            v
                     Processed result
                       /          \
                      v            v
                 Display path    Export path
                      |            |
                      v            v
                   Viewer         File

This diagram expresses responsibility boundaries, not a requirement for a large
class hierarchy.

The implementation should remain as small as possible while keeping these
boundaries clear.

## Responsibility boundaries

### UI

The UI expresses user intent.

Examples:

- enable or disable a node;
- change a parameter;
- add, remove or reorder a node;
- start export;
- change a display setting.

The UI should not know how to cancel a render, acquire graph ownership, wait for
an export, manipulate renderer pending flags or decide which worker must wake.

UI code should not implement sequences such as:

    wait for renderer
    mutate document/graph
    schedule preview

Those sequencing obligations belong below the UI boundary.

### Document / graph API

The document/graph layer owns what the edit means.

It owns or coordinates:

- source/document state;
- node identity;
- graph topology;
- node enabled/bypass state;
- parameter changes at the document level;
- complete graph-edit transactions;
- colour interpretation state;
- persistence-facing state.

Structural edits must go through this boundary rather than directly mutating a
live node vector from arbitrary UI code.

A complete graph edit should be applied as one operation and should produce one
final invalidation/request decision.

### Processors

A processor owns how an image operation is performed.

Processor backends include:

- native C++;
- OpenFX;
- CTL;
- future DCTL;
- future LUT/CLF-backed processing.

Processors expose parameters and image-processing behaviour. They should not
know how RawNode schedules preview work, restores export previews, updates UI
status or persists the document.

Backend-specific synchronization remains inside the backend adapter where
required.

### Graph evaluator

The evaluator owns how processing dependencies are executed for one requested
result.

The target evaluator is intentionally simple:

- sequential;
- dependency ordered;
- evaluates only nodes reachable from the requested document output;
- executes a shared upstream node once per evaluation;
- retains intermediate buffers only while downstream consumers need them;
- fails the complete evaluation if a required node fails;
- checks cooperative cancellation between operations where practical.

This is not a persistent node-cache system.

### Render runtime

The render runtime owns execution policy and lifetime safety.

Its responsibilities should include:

- preview worker lifetime;
- start/stop/join lifecycle;
- preview/export/edit execution exclusivity;
- image-render demand;
- display-refresh demand;
- cooperative cancellation/supersession;
- shutdown draining;
- safe result publication;
- owned export execution/lifecycle.

The current internal flags may continue to exist if they remain the simplest
correct implementation. They are not part of the feature API.

Encapsulation is only useful if callers no longer have to understand the
underlying wait/mutate/reschedule protocol.

The render runtime must not become a general-purpose job system.

### Display path

The display path consumes the last successful processed result and converts it
for presentation.

Display/monitor changes should not rerun expensive image processors when the
processed result itself is still valid.

The viewer owns view-only behaviour such as pan/zoom when that behaviour does
not require a different processed preview.

### Export path

Export consumes the same document graph/evaluator at full resolution, but it is
a separate evaluation from the preview.

Initially, export remains serialized against preview execution because processor
instances, especially OpenFX instances, are not assumed to be safely reusable
concurrently.

Export execution must have an owned lifecycle rather than a detached thread
borrowing application state indefinitely.

An export represents the accepted document state when Export is confirmed.
Parameter edits are disabled while the export owns the live processor instances.

Do not introduce parameter snapshots, queued edits or cloned processor graphs
unless non-blocking background export becomes a deliberate product requirement.

### Persistence

Persistence serializes document state, not renderer state.

It owns:

- stable node identity;
- processor identity/backend;
- parameters;
- enabled/bypass state;
- colour/document settings;
- graph topology once branching is enabled;
- future selected document output.

Renderer pending flags, busy flags, cancellation tokens and worker state must
never become persistent edit state.

## Current safe renderer baseline

PRs #32-#34 established behaviour that must survive the architectural
transition unless a later change deliberately replaces it with a stronger
contract.

Required invariants include:

- preview rendering must not use processors being destroyed or replaced;
- graph mutation must not race active processor execution;
- export must safely own the live processor instances it uses;
- preview and export must not concurrently access those instances when unsafe;
- display-only refresh must not consume or cancel required processor work;
- explicit cancellation/idle barriers must actually drain the work they promise
  to cancel;
- export completion must restore a current preview;
- restored preview work must not overwrite export success/warning/error status;
- failed or cancelled work must not resurrect obsolete work;
- shutdown must drain execution owners before graph/application destruction.

These invariants are more important than preserving the current implementation
shape.

## Control-thread contract

Document and graph mutation are controlled from one UI/control thread.

This is an explicit architectural contract for the current application.

Debug builds should assert the contract at important mutation boundaries where
practical.

Do not design current graph APIs as though arbitrary concurrent mutation is
supported.

Worker/export threads may execute processing but must not initiate document
mutation transactions that would wait on their own execution ownership.

## Graph target

The current source still evaluates an ordered vector chain. The target graph
adds only the topology needed for future masks/compositing.

The first graph-capable representation should provide:

- stable node IDs;
- named input/output ports;
- simple port roles such as image and mask;
- explicit upstream endpoint references;
- fan-out;
- cycle rejection;
- required/optional input validation;
- one authoritative document output.

The initial UI may remain a list.

Graph/list views, if both exist, must represent the same underlying graph rather
than maintain separate processing models.

Existing native, CTL and OpenFX processors may remain unary processors at first.
RawNode graph support does not require immediate support for arbitrary OpenFX
multi-clip effects.

### Evaluation

Topology and executable evaluation should land together.

Do not create a second speculative graph structure that the renderer ignores.

The first DAG evaluator should:

- remain sequential;
- preserve current linear output when given an equivalent linear graph;
- execute dependencies in topological order;
- avoid repeated evaluation of shared upstream nodes;
- keep whole-reachable-graph invalidation;
- use simple per-evaluation consumer/buffer lifetime.

Do not add parallel branch execution, persistent node caches, fine-grained
dependency hashes, general ROI scheduling or a job system as part of this work.

## Masks, opacity and compositing

Masks are image-processing data, not a new renderer/scheduler domain.

The graph must be capable of representing a mask as an explicit input to an
operation rather than requiring renderer-specific mask state.

The UI may still present a mask as belonging to an adjustment even if the
internal graph represents mask generation and mixing explicitly.

Keep these concepts distinct:

- local-adjustment/effect strength;
- scalar mask/coverage;
- image alpha;
- layer compositing.

A node-opacity UI must not force every processor backend to implement masking
internally.

The exact first mask graph/UX contract remains a product decision before mask
feature implementation. The architecture must not prevent graph-native masks
and compositing.

## Colour semantics

RawNode is intended for serious colour-managed photographic workflows.

Unless an operation explicitly requires otherwise:

- preserve floating-point precision;
- preserve negative RGB;
- preserve RGB above 1.0;
- avoid unintended clipping;
- preserve alpha;
- keep colour-space/primaries and transfer-function/gamma concepts distinct;
- avoid silently introducing display-referred behaviour into scene-referred
  processing.

Image meaning is more than numeric RGBA values. As branching/compositing is
introduced, graph values must carry enough semantic information to determine
whether independently processed branches are compatible.

The current output-tag behaviour remains a manual interpretation contract until
a deliberate colour-management change replaces it. Do not silently turn it into
a conversion operation during architecture refactors.

Monitor/display conversion belongs after the processed-image boundary.

## Alpha

No permanent global alpha representation is approved yet.

Existing processing often behaves like straight/unassociated RGB with alpha
preserved separately, but TIFF/EXR/OpenFX boundaries require focused validation
before this becomes a formal contract.

The leading candidate remains:

- straight/unassociated RGBA at general processor boundaries;
- premultiplied arithmetic inside operations that require it, such as
  compositing/filtering;
- explicit adaptation at I/O and OpenFX boundaries.

This remains pending until transparent compositing or alpha-carrying I/O makes
the decision necessary.

Mask representation is a separate decision from image alpha.

## Invalidation and scheduling

Do not create a scheduler domain per feature.

The broad conceptual distinction remains:

    source / processor / parameter / topology / mask / blend change
        -> processed image work required

    display / monitor interpretation change
        -> display work required

Viewer-only changes may require neither.

Whole-graph rerendering is acceptable initially.

A revision/key system is not an architectural goal by itself. Introduce result
identity or revision machinery only when a concrete requirement makes it
simpler than the existing demand/cancellation model.

Freshness, work demand, cancellation, execution ownership and status policy are
different concerns. Do not collapse them into one "stale" flag if doing so
recreates lost-work bugs.

## Supersession and publication

Interactive supersession promises eventual replacement of obsolete preview
work.

RawNode does not currently promise the stronger property that an obsolete frame
can never transiently publish after a later edit.

Do not add machinery to enforce that stronger contract unless it becomes a real
observable problem.

## Status policy

Operation outcomes such as export success/failure or node-creation failure
should remain visible.

If a failed operation must restore preview work, that restoration should be
quiet so routine preview progress does not erase the meaningful operation
status.

Status ownership should remain separate from image freshness.

## OpenFX constraints

OpenFX instances are mutable host/plugin objects and must be treated
conservatively.

For now:

- preview/export execution remains serialized for shared processor instances;
- OpenFX processors remain unary in the first graph implementation;
- no parallel branch rendering is assumed safe;
- no processor cloning is required for export;
- parameter/change-action concurrency should be validated with TSan and
  representative real plugins.

Before any future parallel rendering or preview/export concurrency, RawNode must
honour relevant OpenFX render-thread-safety declarations and re-audit host-side
shared state.

## Persistence migration for topology

Sidecar V2 remains the current linear-document format.

When non-linear topology becomes writable, persistence must explicitly encode
connections and output-port identity.

A future format must preserve:

- stable node IDs;
- missing/unavailable processors;
- unknown parameter values where currently preserved;
- explicit connections;
- source/output port identity;
- document output;
- safe copy/paste ID remapping.

Older RawNode builds must not silently reinterpret a branching graph as a linear
chain.

The exact policy for writing V2 for still-linear graphs versus always writing a
new version remains to be decided with the persistence implementation.

## Deliberately deferred complexity

The following are not part of the approved near-term architecture work:

- persistent per-node caches;
- branch-specific invalidation;
- parallel branch execution;
- tile schedulers;
- comprehensive ROI scheduling;
- general-purpose job systems;
- multiple renderer worker pools;
- automatic processor cloning for preview/export;
- multiple simultaneous exports;
- temporal/feedback graphs;
- Vulkan as an application architecture requirement;
- GPU-native graph-result storage;
- automatic colour-space inference for arbitrary OFX/CTL/DCTL code;
- OpenFX multi-clip support;
- intermediate-output viewer selection;
- a dedicated status subsystem;
- a broad revision/evaluation-key framework.

Prefer re-rendering slightly too much before adding another invalidation or cache
domain.

## Approved migration sequence

The architecture should be reached through small behaviour-preserving changes.

1. **Own the export job.**
   Replace detached export lifetime with owned execution and RAII cleanup. Make
   production export directly testable.

2. **Freeze parameter edits during export.**
   Enforce the approved export consistency contract without snapshots or queues.

3. **Centralize graph-edit transactions.**
   Remove caller-managed wait/mutate/reschedule sequences and fix failure
   restoration at the same boundary.

4. **Encapsulate renderer ownership.**
   Once the public protocol has shrunk, own worker lifecycle, execution
   exclusivity, demand, cancellation and shutdown behind one runtime boundary.

### Stabilisation gate after Step 4

Before beginning Step 5:

1. fix the pre-existing OpenFX host multithread lifetime bugs exposed by Step 4
   review (early `multiThread()` completion and an unjoined global worker pool)
   in a separate focused correctness PR;
2. perform a behaviour-preserving human-readability/source-organisation pass so
   file boundaries, function order, naming, comments and lock/ownership structure
   make the architecture legible to a competent C++ programmer;
3. perform the planned whole-architecture checkpoint against RawNode's actual
   product goals and the pinned vkdt reference.

These are stabilisation/review gates, **not additional numbered architecture
steps**. The readability pass must not smuggle architecture or behaviour changes
into cleanup. Architectural problems discovered there should be recorded for the
checkpoint.

5. **Settle the first mask/graph product contract.**
   Decide the first user-visible mask model before graph feature work.

6. **Introduce explicit topology and sequential DAG evaluation together.**
   Preserve equivalent linear behaviour first.

7. **Add versioned graph persistence.**

8. **Define the mask contract and add the first mask path.**
   Alpha/compositing contracts are settled only as the corresponding feature
   requires them.

Each step should remain independently reviewable.

## Source organisation and human readability

Source layout should help a human programmer recover the architecture from the
code rather than requiring knowledge of the project's implementation history.

Prefer:

- one obvious responsibility per file or tightly related file group;
- top-down implementation order: public/domain operations before lower-level
  mechanics where practical;
- related lifecycle, request, mutation, export and worker operations grouped
  conceptually rather than ordered by when they were added;
- domain-oriented names over names that expose incidental implementation
  machinery;
- small, visually obvious lock scopes and comments that state ownership/thread
  invariants;
- comments that explain **why**, invariants and non-obvious constraints rather
  than restating C++ syntax;
- source structure that mirrors the UI/document/graph/runtime/display/export/
  persistence boundaries in this document.

Do not pursue readability by inventing layers, managers or tiny wrappers without
a real responsibility boundary. Formatting is useful but is not a substitute
for conceptual reading order.

The durable source-style guidance lives in `docs/CODE_STYLE.md`.

## Testing requirements

Core architecture changes require tests that exercise production paths, not only
hand-reimplemented equivalents.

For renderer/export/graph work:

- retain deterministic #32-#34 regression coverage;
- prefer barriers/observers over timing sleeps;
- provide a deliberate test observation seam when internals become private;
- run the full self-test suite;
- use ASan/TSan where practical;
- validate failure paths and exception cleanup;
- ensure tests fail when the intended guarantee is deliberately removed;
- test real OpenFX interaction when concurrency behaviour is involved.

## Final design rule

When adding a feature, ask:

    Where does this belong?

Typical answers should be obvious:

    DCTL / LUT / Exposure / CTL / OFX
        -> processor + graph

    Mask
        -> graph data / graph operation

    Blend / compositing
        -> graph operation

    Display profile / monitor transform
        -> display path

    Export format / file encoding
        -> export path

    Sidecar format
        -> persistence

If the answer is instead "add knowledge of this feature to UI, renderer,
persistence, export and several global flags", stop and re-examine the boundary.
