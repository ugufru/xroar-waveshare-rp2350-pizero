# The reference 6809 core (PIZERO-129)

A frozen copy of `lib/xroar_core/src/mc6809/` and `mc680x/mc680x_ops.c` as
they were at commit 8a9032f (2026-10-09), before any speed work on the core
(PIZERO-131). `test/test_mc6809_lockstep/` builds this copy and the live core
into one binary, runs them instruction by instruction against the same
memory, and fails at the first bus cycle or register that differs.

**Do not edit these files.** If upstream XRoar fixes a 6809 bug and the live
core takes the fix, refresh this copy from the live core in the same commit,
and say so in the commit message: the test then proves the optimised core
matches the fixed core, which is the point.

PlatformIO ignores this directory because its name does not start with
`test_`; the lockstep test includes the files by relative path.
