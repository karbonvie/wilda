# AI-agent collaboration notes

Firmware, scripts, and configs in this project were written largely
by AI coding agents under direction and review. The four episodes
below are the ones where that collaboration visibly mattered: bugs
caught, false confidence exposed, decisions argued out. They are
reconstructed from the dated archive and written session summaries;
the raw chat transcripts were not retained. Scrubbed: no
credentials, no hostnames, no third-party names.

## Episode 1: the QSPI regression that fooled the counter
(2026-04-07 to 04-09)

While bringing up QSPI flash, the AI-hardened logger regressed a
proven transport. IO2/IO3 (WP#/HOLD#) had to be held high; the
hardware QSPI block was deactivated to move forward. A flash_errors
counter in RAM reported success while the proven path was broken.
The regression was caught by hardware validation, not by the
counter.
Lesson: a RAM counter is not an instrument; validate against proven
paths on hardware.

## Episode 2: boot-spike energy accounting (2026-04-09 to 04-10)

A 40 s boot energy spike showed up in the sustained logging runs
(18,900 records in 2 h). The accounting attributed it correctly, and
it was eliminated by design in page_batch_chkpt_resume (32,640
records in one sustained run), not trimmed.

## Episode 3: batching vs direct write (2026-04-13 to 04-16)

The choice between direct per-record writes and checkpointed page
batches was argued against the measured 8 mA / 15 ms burst cost and
the power-loss-anytime requirement. Page batching with checkpoints
won; see docs/decisions/ADR-003-page-batching.md.

## Episode 4: vertical slice before extension (2026-04-14)

After the QSPI regression, the decision was taken to freeze a
canonical 13-field export contract and complete one end-to-end
slice, capture to flash to export, before any further extension;
see docs/decisions/ADR-004-vertical-slice.md.
