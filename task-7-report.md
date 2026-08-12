# Task 7 PMU review fixes

- Demo and multiring consumers now read the existing Fabric PMU snapshot's
  `l2.total` statistics; no Fabric L2-buffer getter was restored.
- Existing unicast, multicast, and scatter performance scenarios now assert
  L2 response admission, carrier recipients, DAT bytes, carrier classes, and
  the 128B/256B/512B carrier buckets from known request actions.

## Verification

- `git diff --check` passed.
- `rg -n "l2_buffer_stats\\(" --glob '!*.pyc'` returned no matches.
- Compilation and runtime tests were not run because this worktree has no
  `testprj` directory.
