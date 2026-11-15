# ADR-005: Measurement discipline: shunts, peak-catcher, probe detached

Date: 2026-03-16 to 2026-03-31. Status: accepted.

## Context

The INA219's 68 ms averaging window hid 8 mA / 15 ms flash-write
bursts. A J-Link left attached added roughly 170 uA of backfeed. A
multimeter alone cannot resolve uA currents.

## Decision

Average current is measured with INA219 shunt instrumentation
across 0.1 / 1 / 10 ohm shunts, cross-checked with a multimeter.
Bursts are caught by a peak-catcher sampling in 532 us windows.
Final sleep-floor numbers are taken with the debug probe detached.
Traces are not retained; values are recorded in dated session logs.
PPK2 profiling is planned.

## Consequences

The 0.47 mA to 32 uA optimization arc is measured, not estimated.
Average and peak are always reported as distinct numbers. The
archive keeps the session readings that produced each figure.
