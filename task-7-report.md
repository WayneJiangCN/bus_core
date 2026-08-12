# Task 7 PMU review fixes

- Demo and multiring consumers now read the existing Fabric PMU snapshot's
  `l2.total` statistics; no Fabric L2-buffer getter was restored.
- Existing unicast, multicast, and scatter performance scenarios now assert
  L2 response admission, carrier recipients, DAT bytes, carrier classes, and
  the 128B/256B/512B carrier buckets from benchmark inputs and topology.
- Same-line scatter expectations use the physical 512B carrier produced by
  the covered sector range, rather than the logical 128B/256B request size.
- The free-running multicast/scatter benchmarks retain timing-dependent total
  aggregation counts, but assert topology-derived recipient totals, physical
  DAT-byte/bucket relationships, carrier classes, and response/carrier bounds.
- Aggregation-wave tests derive exact responses, carriers, recipients, DAT
  bytes, and buckets from the fixed master, request, line, and V-Ring geometry;
  no observed HA/L2 PMU field supplies an expected total.
- A deterministic PMU event test injects one known 128B unicast, 256B
  multicast, and 512B scatter carrier and asserts exact response, carrier,
  recipient, DAT-byte, class, and bucket totals.
- Private and cross-line unicast reads likewise derive exact response,
  carrier, recipient, DAT-byte, V-Ring, and size-bucket expectations directly
  from the configured benchmark workload; 1024B carriers use the `other`
  bucket.

## Deterministic expectations

| Scenario | Responses | H carriers | Recipients | Physical DAT bytes | Nonzero bucket |
| --- | ---: | ---: | ---: | ---: | --- |
| Private/no-merge read 128B | 65536 | 65536 | 65536 | 8388608 | 128B=65536 |
| No-merge read 256B | 32768 | 32768 | 32768 | 8388608 | 256B=32768 |
| No-merge read 512B | 16384 | 16384 | 16384 | 8388608 | 512B=16384 |
| Cross-line unicast read 1024B | 8192 | 8192 | 8192 | 8388608 | other=8192 |
| Wave scatter read 128B | 64 | 64 | 256 | 32768 | 512B=64 |
| Wave scatter read 256B | 64 | 64 | 128 | 32768 | 512B=64 |
| Wave multicast read 128B | 32 | 64 | 256 | 8192 | 128B=64 |
| Wave multicast read 256B | 16 | 32 | 128 | 8192 | 256B=32 |
| Wave multicast read 512B | 8 | 16 | 64 | 8192 | 512B=16 |

The free-running aggregated tests have exact recipient totals (65536 for 128B
and 32768 for 256B), while response/carrier totals remain runtime aggregation
outcomes and are checked through independent bounds and physical-size
relationships.

## Verification

- `git diff --check` passed.
- The self-derived expected-value scan found no `expected = l2.*`/`ha.*` or
  exact-helper calls fed by `l2.h_carriers`.
- `rg -n "l2_buffer_stats\\(" --glob '!*.pyc'` returned no matches.
- Compilation and runtime tests were not run because this worktree has no
  `testprj` directory.
