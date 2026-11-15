# ADR-001: Tag platform is nRF52 with Zephyr/NCS

Date: 2025-09-11, reaffirmed 2026-01-23. Status: accepted.

## Context

The project began (2025-08) as an STM32 research phase with a first
LoRaWAN proposal targeting STM32WL55, including a gateway concept
(2025-09-16) and a STOP2 bring-up (2025-10-04). Zephyr support for
STM32WL never matured enough to bet the tag on it.

## Decision

The tag platform is Nordic nRF52 with Zephyr (NCS). The STM32WL
direction is formally dropped (2026-01-23) and the gateway role moves
to a Raspberry Pi 400 + RAK5146 concentrator. The STM32WL55 bring-up
stays in the archive as a documented dead end.

## Consequences

Mature low-power states and tooling, at the cost of learning
Nordic-specific behavior (SAADC quirks, DC/DC, J-Link backfeed). The
radio becomes a separate device, which leads to ADR-002.
