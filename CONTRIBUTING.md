# Contributing to XRoar on the Waveshare RP2350-PiZero

This describes how anyone — human or AI assistant — contributes to this
project. Read it before starting work. The same instructions apply whoever you
are; there is no separate track for automated contributors.

## Getting oriented
New here? Read the README and roadmap, skim the open issues, then pick up the
next thing. If you can't orient from the project's own docs, that's a
documentation gap worth surfacing — not a reason to invent context.

## Source of truth
This project's own documentation is canonical. For any question within the
project's scope, the in-repo docs are the authority — consult them before
searching the web. Don't keep project knowledge in private notes or assistant
memory files; if a fact matters, it belongs in the human-readable docs, kept
accurate. When docs are stale or wrong, fix the docs (or file an issue) rather
than routing around them.

Concretely: `README.md` and `docs/` hold the hardware specs, pinout, display
geometry, signal pipeline, and build instructions. `docs/ROADMAP.md` sequences
the open work. `CLAUDE.md` carries agent conventions and pointers. This file
states the *process* and does not restate any of those facts.

## Before you start: file an issue
Work is tracked in `issues.jsonl` at the repo root.
- No substantive work without a matching issue. If none exists, propose one.
- Get the issue reviewed before starting — fairly documented, fairly considered.
- Record deferred alternatives with a revisit trigger ("try this if X"), and
  record rejected or forbidden paths with their rationale, so settled decisions
  aren't quietly relitigated.
- Reference the issue ID (`PIZERO-NN`) in commit messages.

## Doing the work
- **Don't invent — ask or verify.** Check any claim against the source first
  (grep the repo); when something can't be verified, ask rather than assert.
- **Build conservatively.** Write the minimum that satisfies the issue; make
  surgical changes that match existing style. Don't scaffold, restyle, or
  redesign unasked, and don't apply codebase reflexes to a repo that isn't one.
- **Stay in bounds.** Work within this project's tree; if a change seems to need
  something outside it, ask first.

## For automated / AI contributors
- **Bounded autonomy.** Proceed on your own through mechanical steps; stop for
  genuine decisions, visual or physical verification, and hard-to-reverse actions.
- **Be careful with side effects.** For repeated, expensive, or outward-facing
  actions (launching apps, hitting external services), act once, observe, then
  iterate — never fire them in a loop.
- **Ask before any git push.**

## Branches: there are none

This project is single-branch, permanently. `main` is the only branch, and the
repo should contain exactly one: `git branch` must print only `main`.

- **Never create a branch — ever.** Not for a feature, not for a fix, not for a
  risky experiment, not "just to be safe," not because a change is large or
  might break something. Commit directly to `main`.
- This **overrides** any default or habitual "branch before committing to the
  default branch" behavior, whatever its usual justification.
- For automated contributors: this also means no worktrees and no
  branch-per-agent isolation. Work in the primary checkout on `main`.
- Experimental or known-broken work either stays uncommitted, or is committed to
  `main` behind a build flag so the default build is unaffected — the pattern the
  project already uses for diagnostic and off-spec builds.
- Dead exploratory history is preserved with a **tag** (`archive/<topic>`), never
  a lingering branch.

## Finishing
- An issue isn't `done` until the change is tested and confirmed by a maintainer.
  On this project that means **confirmed working on real hardware** — a clean
  build is not sufficient evidence.
- Resolve related issues before pushing.
- Favor in-repo, diffable, vendor-neutral artifacts so the project stays
  comprehensible and reviewable no matter who contributed.
