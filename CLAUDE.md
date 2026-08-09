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
