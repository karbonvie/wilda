# wilda

Ultra-low-power wildlife telemetry tags. Bench prototype, version zero.

Personal, self-funded, open-source project, 2025-2026. Battery-powered
LoRaWAN sensor tags on Nordic nRF52 hardware, with dated evidence for
every milestone. Built and measured on a bench. Never deployed on
animals.

## Scope, stated plainly

What works, on the bench, with measurements behind it:

- Sleep-current optimization arc on the nRF52840 tag: 0.47 mA down to
  32 uA average (v8), via pin parking, DC/DC mode, console-off, and
  flash deep power-down. Average current measured with INA219 shunt
  instrumentation, cross-checked with a multimeter; traces not
  retained; values recorded in dated session logs; PPK2 profiling
  planned.
- Interrupt-driven event capture (event_wake) on AIN0/AIN2 inputs,
  validated on the nRF52 DK, then carried to the Feather tag.
- Flash logging that scales: checkpointed page batches, 32,640 records
  in one sustained run; 32-byte CRC records with time anchors and RTC
  hooks; a canonical 13-field export contract, with 1,459 records
  recovered across boots using boot_id.
- Peak-catcher instrumentation: 15 ms / 8 mA flash-write bursts were
  caught with a 532 us sampling window after the INA219's 68 ms
  averaging window hid them.
- A root-caused QSPI bring-up regression (IO2/IO3 = WP#/HOLD# must be
  held high) that briefly broke a proven transport, and a root-caused
  40 s boot energy spike, eliminated by design.
- End-to-end LoRaWAN: self-hosted gateway (Pi 400 + RAK5146), tags on
  The Things Network, an autonomous Zephyr-to-RAK3172 control loop
  FSM, wake from 1.7 uA modem sleep, uplink confirmed over TTN.
- Designed and used a battery-powered mobile LoRaWAN beacon as a
  coverage probe against a self-hosted gateway; informal coverage
  checks were performed; quantitative walk data was not retained.

What this is not:

- Not deployed on animals, not field-hardened: no enclosures, no IP
  rating, no long-term reliability data.
- No multi-month autonomy claimed, no sub-5 uA floors claimed. Battery
  life was not extrapolated from bench numbers.
- No on-device machine learning, and none is planned in this repo.
- No dashboards, no cloud stack. The MQTT catcher in scripts/ was a
  bench utility for one validation session, not a deliverable.

## Hardware

- Tag: nRF52840 (Adafruit Feather nRF52840 Express); earlier event_wake
  work on an nRF52 DK.
- LoRa modem: RAK3172 breakout (nb1 and nb2 roles).
- Gateway: Raspberry Pi 400 + RAK5146 concentrator, self-hosted,
  registered on The Things Network.
- Instruments: INA219 boards with 0.1 / 1 / 10 ohm shunts, AD2,
  multimeters, UNO R4 + 74AHC125 level-shifted pulse source, J-Link,
  CFG250.
- Next hardware: nRF54LM20A (Seeed XIAO), boards in hand.

## How the work was done

Stated plainly: firmware, scripts, and configs were written largely by
AI coding agents under direction and review. Architecture, hardware
bring-up, provisioning, all instrumentation, and all validation were
hands-on. Every milestone was verified on hardware before being
archived. Curated excerpts of the agent sessions that caught real
bugs are in docs/notes/ai-agent-notes.md.

## Repository layout

- firmware/ : eight milestone snapshots, point-in-time copies of the
  tag firmware, credentials replaced with placeholders.
- docs/milestones.md : the authoritative dated timeline.
- docs/decisions/ : short records of the design decisions that matter.
- docs/images/ : screenshots and bench photos with one-line captions.
- docs/notes/ : curated AI-agent collaboration notes.
- scripts/ : bench utilities (mobile beacon, MQTT catcher, TTN payload decoder,
INA219 peak-catcher, UNO R4 pulse generator, modem UART test).

## Timeline and provenance

This repository was assembled in November 2026 from a dated local
archive. For pre-repo work, the dates live in docs/milestones.md and the
local dated archive, which is not published; git commit dates before 2026-09-06
are archival, not work dates.

## Naming

Research started in 2025 as wildlifethrive, carried an informal dev
alias a10b0r0, and was renamed wilda when this repo was created
(2026-09-06). Older zips and documents may use the earlier names.

## Status

Version zero, bench-proven. Next: nRF54LM20A (Seeed XIAO nRF54)
bring-up, started; PPK2 current profiling planned.

## License

MIT, see LICENSE.
