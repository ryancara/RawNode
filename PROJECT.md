# RawNode Project

RawNode is a lightweight, cross-platform, non-destructive RAW photo editor built around a modular processing pipeline.

This document defines the product direction and high-level constraints. Implementation details belong in `ARCHITECTURE.md`; staged work belongs in `ROADMAP.md`; persistent edit-state details belong in `SIDECAR.md`; important choices and their rationale belong in `DECISIONS.md`.

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

Opening a folder is the workspace. RawNode must not require a Lightroom Classic-style catalogue/database or a project file.

### Sidecars are the source of truth

Each edited image has a sidecar containing everything required to reconstruct its edit. A small optional folder-level UI-state file may remember things such as the selected image, sort order, filmstrip position, or panel layout, but it must never contain essential image-edit data.

### Non-destructive

Original image files are never modified. Removing the sidecar returns the image to its unedited state.

### Lightweight

RawNode is not intended to become a DAM, cloud platform, asset-management system, Photoshop replacement, or mandatory-library workflow.

### Cross-platform

Primary targets are macOS, Windows, and Linux. Platform-specific code should sit behind clear abstractions.

### Open and extensible processing

Photographic adjustments should be implemented as modular processors wherever practical rather than permanently hard-coded into the application.

## Processing model

RawNode should use a graph-capable internal representation, while initially keeping user processing serial and simple.

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

Graph branching is not required initially, but the internal model should avoid making it impossible later.

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

Possible future features include masks, local adjustments, groups, AI-assisted masking, and graph branching.

## Current technology direction

The current preferred direction is:

- core/application: C++;
- UI: Dear ImGui + GLFW;
- build system: CMake;
- current RAW decoder: LibRaw initially;
- processing: OFX + CTL + DCTL compatibility;
- edit persistence: JSON sidecars.

These are current choices, not permanent constraints. Change them only for a concrete technical benefit and record significant changes in `DECISIONS.md`.

## Immediate objective

The first architectural milestone is to fork OFX Raw Host and change its OFX-specific node representation into a generic processor/node architecture **without changing existing behaviour**.

If that refactor remains clean, the fork becomes the long-term foundation. If it exposes severe architectural limitations, reassess before adding CTL, DCTL, alternate RAW decoders, masks, or other major features.

## For collaborators and AI assistants

Before proposing substantial architectural changes, read:

1. `PROJECT.md`
2. `ARCHITECTURE.md`
3. `ROADMAP.md`
4. `DECISIONS.md`
5. `SIDECAR.md`

Prefer small, reversible changes. Do not silently redefine product goals or introduce catalogue/project/cloud concepts that conflict with this document.
