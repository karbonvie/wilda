# ADR-002: Two-board split, nRF52840 logger + RAK3172 modem

Date: 2026-01-23. Status: accepted.

## Context

After dropping the single-chip STM32WL line, a LoRaWAN stack had to
come from somewhere. Writing one was out of scope: the goal was bench
validation of the telemetry chain, not radio stack development.

## Decision

The nRF52840 Feather runs the application: event capture, flash
logging, power management. A RAK3172 module (RUI3, certified LoRaWAN
stack) is held in deep sleep and woken over UART AT commands only
when an uplink is needed.

## Consequences

Two sleep domains to instrument: the tag at 32 uA average (v8), the
modem at 1.7 uA in AT+SLEEP, knock-wake demonstrated (2026-05-11).
The power budget includes modem wake bursts. This split is the
baseline the nRF54 single-board line aims to beat.
