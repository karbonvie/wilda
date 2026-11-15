# ADR-004: Canonical 13-field contract, vertical slice before extension

Date: 2026-04-14. Status: accepted.

## Context

Record formats had drifted across tiers, making export brittle. In
the 2026-04-08/09 QSPI episode an AI-hardened logger had regressed a
proven transport, and a flash_errors RAM counter gave false
confidence in the new code.

## Decision

Freeze a canonical 13-field export contract and build a vertical
slice, capture to flash to export to CSV end to end, before any
further extension. Broken intermediate versions stay in the archive
as negative results.

## Consequences

Canonical export v2 recovered 1,459 records across boots using
boot_id. Later work extends the slice instead of refactoring it.
