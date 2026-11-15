# ADR-006: AI-agent role split, with the regression lesson

Date: standing since 2025-10, reinforced 2026-04-09.
Status: accepted.

## Context

Firmware, scripts, and configs in this project were written largely
by AI coding agents. In the 2026-04-08/09 QSPI episode an
agent-hardened logger regressed a proven flash transport, and a
flash_errors RAM counter gave false confidence.

## Decision

Agents write code under direction and review. Architecture,
hardware bring-up, provisioning, instrumentation, and validation are
hands-on. Every milestone is verified on hardware before being
archived; regressions are kept as dated evidence, not deleted.

## Consequences

Broken snapshots are preserved as negative results. Curated,
scrubbed collaboration notes live in docs/notes/ai-agent-notes.md.
