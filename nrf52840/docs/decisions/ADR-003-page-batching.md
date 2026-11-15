# ADR-003: Persistence is checkpointed page batching

Date: 2026-04-10, sparse events and checkpoint16 on 2026-04-13.
Status: accepted.

## Context

Logging had to survive power loss at any instant. Direct writes were
simple but produced 8 mA / 15 ms bursts and were vulnerable
mid-record. The tier progression (Batch&Blast, Boot_Select&DataDump,
tier4 maintenance branching) explored the space under measurement.

## Decision

Records are accumulated in RAM and written in page batches with a
checkpoint (page_batch_chkpt_resume), with sparse-event support.

## Consequences

32,640 records in one sustained run; the 40 s boot energy spike was
eliminated by design rather than trimmed; write bursts amortized;
checkpoint logic is the main new complexity.
