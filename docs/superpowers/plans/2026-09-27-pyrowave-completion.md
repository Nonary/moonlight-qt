# PyroWave completion timing implementation plan

**Goal:** Establish the shared Linux PyroWave frame's GPU completion before VRR scheduling, and prevent CPU-only output timestamps from training GPU flip protection.

**Architecture:** Keep each frame's decode timeline value with its pool reference. Wait for that value through the existing renderer decode-wait hook, with a bounded timeout and renderer recovery on failure. Preserve the output-render wait and current ownership rules.

**Scope:** Work in the current checkout so the user's existing uncommitted fixes remain part of the local build. Do not commit, push, restart streams, or deploy Windows packages.

- [x] Add a real Vulkan regression for pending completion, per-frame values, timeout, cloned frames and invalid inputs; observe failure with the missing wait.
- [x] Implement the pool wait and Linux renderer hook. Only observed GPU completion can train decode GPU cost; already-ready CPU output must not overwrite it.
- [x] Run pool/codec and VRR regressions, update architecture, and review the focused diff.
- [x] Build the local application in moonlight-dev, smoke-test and record its hash. Visible smoothness requires a subsequent live stream.

Review correction: explicitly carry the PyroWave output semantics so synchronous readback retains its completion samples. Restricted producer annotation to Linux; Windows sampling remains unchanged. The synchronous fallback regression failed before this correction. A fresh reviewer found no remaining blockers.

Validation: 26 test executables plus replay help passed (27 checks). Clean application build and offscreen help passed. Evidence: `build/pyrowave-completion-validation/validation.json`. Live visual verification remains outstanding.
