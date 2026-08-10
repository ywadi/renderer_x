# Repository policy

This repository is pushed to a public remote: `git@github.com:ywadi/renderer_x.git`.

**No AI attribution of any kind in this repo's history or remote-visible content.**

- Never add "Co-Authored-By: Claude" (or any AI attribution) to commit messages.
- Never sign, countersign, or otherwise mark commits, tags, PRs, issues, or
  code comments as authored/co-authored/reviewed by an AI assistant.
- Never list yourself as a contributor anywhere in the project (README,
  CONTRIBUTORS, package metadata, changelogs, etc.).
- Commit author/committer identity stays whatever the user's local git
  config already provides — do not override it.

This overrides the default commit-message template used elsewhere.

## Do not reinvent the wheel

This project's standing engineering rule: **prefer ready-made libraries or
ported implementations over writing subsystems from scratch.** Before
implementing any nontrivial piece of logic (parsers, allocators, math,
loaders, compilers, protocol handling, etc.), check whether a well-established
library already solves it, or whether a known open implementation can be
ported. Only write it from scratch when no reasonable ready-made option
exists, and say so explicitly when that's the call being made. This applies
to every task, every subagent, and every layer of the renderer — not just
the RHI/toolchain layers that already lean on SDL3, vk-bootstrap, VMA,
volk, GLM, spdlog, doctest, and Slang.

## Performance is an exit criterion

RendererX is middleware: its consumers' games are the real workload, so the
engine **leads on performance instead of waiting for evidence from its own
small samples** — it builds its own stress cases (synthetic high-draw-count
benchmark scenes) and proves scalability proactively.

- From Phase 4 onward, **every phase exits with published benchmark numbers**
  (desktop AND Steam Deck — the hardware floor) for its stress/exit samples,
  and CI carries **performance regression gates** on those numbers alongside
  the correctness gates. A performance regression blocks a phase exit the
  same way a failing test does.
- Design the fast path as the default path: instanced/batched submission,
  pooled global geometry buffers, bindless access, minimal derived barriers.
  Per-object state churn and retrofit-later designs are rejected at review.
- Features above the Vulkan 1.3 baseline (mesh shaders, hardware RT, etc.)
  are **optional capabilities with a fallback**, never baseline requirements
  — but the fallback path is engineered to the same performance bar.
- Profiling instrumentation (Tracy) is part of the toolchain from Phase 4:
  performance claims in reports must be measured, not asserted.

This binds every task, every implementer, and every reviewer, like the rules
above. See `docs/superpowers/specs/2026-08-10-phase4-seed-notes.md` and the
deferred-registry entries in
`docs/superpowers/specs/2026-08-09-toolchain-platform-rhi-design.md`.
