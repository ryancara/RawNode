# RawNode Project

RawNode is a lightweight, cross-platform, non-destructive RAW photo editor built around a modular processing pipeline.

This document defines the product direction and high-level constraints.
Authoritative technical guidance lives in `docs/ARCHITECTURE.md`, accepted
decisions in `docs/DECISIONS.md`, current state in `docs/PROJECT_CONTEXT.md` and
coding guidance in `docs/CODE_STYLE.md`. Root-level `ARCHITECTURE.md` and
`DECISIONS.md` are historical pointers. `ROADMAP.md` tracks staged work;
`docs/STEP7_PERSISTENCE_DESIGN.md` specifies graph-native persistence, with
`SIDECAR.md` as the overview and `docs/workspace-format.md` for global settings.

## Core idea

RawNode should do relatively little itself. Its primary responsibilities are to:

- open folders and images;
- develop RAW data into a clean scene-linear working image;
- host and execute modular processing stages;
- display the result accurately;
- persist image-specific settings in sidecar files; and
- export finished images.

The processing ecosystem should ultimately support:

- OpenFX (OFX);
- CTL; and
- DCTL or a clearly documented DCTL-compatible subset/runtime.

The architecture must remain modular enough that individual backends, decoders, renderers, or UI components can be replaced later without redesigning the whole application.

## Product principles

### Folder-based, never catalogue-based

Open folders and browse photographs without a catalogue/database or project
file. Folder/photo selection is transient; no folder workspace file is used.

### Sidecars are the source of truth

Successfully opening an editable photograph establishes or restores a complete
grade, including Source-only. New grades are saved to `.rawnode.json` after
opening succeeds; existing grades need no immediate rewrite. Save failures retain
the in-memory grade with a warning. Grade and photo metadata are independent
sections. Global `preferences.json` and ImGui `layout.ini` own
application settings and layout; folder-level settings persistence is retired.

### Non-destructive

Edits are stored separately from original images. Removing the sidecar removes
the saved grade and photo metadata. Exporting over an editable raster source
path remains a known overwrite hazard; see `docs/PROJECT_CONTEXT.md`.

### Lightweight

RawNode is not intended to become a DAM, cloud platform, asset-management system, Photoshop replacement, or mandatory-library workflow.

### Cross-platform

Primary targets are macOS, Windows, and Linux. Platform-specific code should sit behind clear abstractions.

### Open and extensible processing

Photographic adjustments should be implemented as modular processors wherever practical rather than permanently hard-coded into the application.

## Processing model

RawNode uses one authoritative document graph with typed connections, stable
NodeIds and validated mutation APIs. One sequential evaluator serves preview,
inspection and export. The current List View derives order from topology.

Example serial path:

```text
RAW -> Exposure -> White Balance -> Contrast -> Spektrafilm -> Output
```

The same processing path should eventually be viewable in two ways:

**Graph view**

```text
RAW -> Exposure -> WB -> Contrast -> Spektrafilm -> Output
```

**List view**

```text
1  Exposure
2  White Balance
3  Contrast
4  Spektrafilm
```

List view is not a separate processing system. It is another representation of the same underlying structure.

Branching topology and graph-native persistence are implemented. The switchable
Graph/List editor is the next UI milestone; user-facing masking/compositing
follows in Step 8. Both views use the same graph and evaluation architecture.

## Future local adjustments

Masks and local adjustments are expected future features and should be anticipated in the architecture, but not allowed to complicate early milestones unnecessarily.

Potential future mask sources include:

- brush/paint;
- gradients;
- luminance or colour keys;
- AI-generated subject/background/object masks.

AI should initially be considered a mask-generation capability, not a fundamental processing backend.

## Input and shortcuts

RawNode should eventually support user-assignable control of parameters. The default model should operate on the currently selected node instance so duplicate plugins are unambiguous.

Examples:

- number keys select processing nodes;
- hold a key + mouse wheel adjusts an assigned parameter;
- shortcuts can bypass/reset a node or parameter;
- future MIDI, OSC/TouchOSC, or hardware-controller support should use the same generic parameter/input abstraction.

## Explicit non-goals

RawNode should not require or evolve around:

- catalogue/database management;
- cloud syncing;
- DAM functionality;
- project files;
- mandatory library/import workflows.

Possible future features include masks, local adjustments, groups and AI-assisted masking.

## Current technology direction

The current preferred direction is:

- core/application: C++;
- UI: Dear ImGui + GLFW;
- build system: CMake;
- current RAW decoder: LibRaw initially;
- processing: native + OFX + CTL; DCTL compatibility is planned;
- edit persistence: JSON sidecars.

These are current choices, not permanent constraints. Change them only for a concrete technical benefit and record significant changes in `docs/DECISIONS.md`.

## Current milestone

The original backend-neutral refactor and architecture Steps 1–7 are complete.
Step 7 PRs 1–5 are merged (#63, #65, #66, #68, #69). Next is the switchable
Graph/List editor (D047), followed by Step 8's first concrete masking path.
Outstanding manual platform checks remain in `docs/STEP6_MANUAL_VALIDATION.md`.

## For collaborators and AI assistants

Before proposing substantial architectural changes, read:

1. `docs/PROJECT_CONTEXT.md`
2. `docs/ARCHITECTURE.md`
3. `docs/DECISIONS.md`
4. `docs/CODE_STYLE.md`
5. `ROADMAP.md` and `docs/STEP7_PERSISTENCE_DESIGN.md` for the relevant milestone

Prefer small, reversible changes. Do not silently redefine product goals or introduce catalogue/project/cloud concepts that conflict with this document.
