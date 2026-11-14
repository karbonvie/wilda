# Milestones

Authoritative dated timeline of the project. Assembled in November
2026 from a dated local archive: zip names, session logs, and this
document carry the original work dates. Git commit dates for
pre-2026-09-06 material are archival.

Notes on the record:
- The nrf52dk_ prefix on the tier2-tier4 logger zips is legacy naming
  carryover: that logger work was done on the nRF52840 Feather. The
  2025-10 to 2026-01 event_wake work was done on the nRF52 DK.
- The 2026-03-19 blog header reading "20260819" is a typo; the
  archive file date of 2026-03-19 is correct.
- Two intermediate FSM zip backups (20260511, 20260519) were removed
  from the repo during the pre-publication credential scrub; their
  content survives through the scrubbed extracted folder.

## 2025: research, first measurements, event_wake

- 2025-08-26: wildlifethrive research phase begins (STM32 direction).
- 2025-08-30: first LoRaWAN proposal written.
- 2025-09-11: pivot to nRF52 dongle; first current measurements
  (LED paths 6.3 / 4.7 / 0.61 mA; AD2 + shunt + multimeter).
- 2025-09-16: STM32WL55 gateway concept.
- 2025-10-04: STM32WL55 bring-up (STOP2 low-power mode).
- 2025-10-13: dongle-to-DK porting guides written.
- 2025-10-31 / 12-03: event_wake validated on the DK (AIN0+AIN5);
  12 mA idle / 36 mA active whole-DK; all_ok_final zip archived.
- 2025-12-17: Master System Architecture document.

## 2026-01: DK stabilization, Feather transition begins

- 2026-01: debugging burst, AIN0+AIN2 event capture stabilized
  (dated zips still_work_in_progress, tried_rtt_jlink_debug_but_failed,
  ain0_nearly_ok, then ain0_ain2_work_all_ok on 01-21).
- 2026-01-22: ki_polished_ok snapshot archived.
- 2026-01-23: RAK procurement plan; Feather transition decision;
  STM32WL no-Zephyr decision recorded.

## 2026-02 to 03: Feather bring-up and current optimization

- 2026-02-10: RAK order placed (CA$239.84, plus CA$28.06 MHF1 reorder).
- 2026-03-03: Feather bring-up; J-Link SWD after UF2 rejected.
- 2026-03-10: board_pins.h split, v2.
- 2026-03-11: verified node v3 (240 mV/pulse with the pulse source).
- 2026-03-14 / 15: v4/v5 expose the SAADC input_positive macro bug
  (AIN2 silently follows AIN3).
- 2026-03-16: v6 fixes it (raw AIN index, not HAL bitfields); INA219
  multi-shunt instrumentation built; first reading ~0.47 mA.
- 2026-03-19: v7 pin parking, DC/DC, console-off; 33-110 uA.
- 2026-03-23: v8 stable 32 uA average + flash deep power-down; J-Link
  ~170 uA backfeed rule identified (measure with J-Link detached).

## 2026-03 to 04: persistence tiers, records, the QSPI regression

- 2026-03-30 / 31: tier2 Batch&Blast and tier3 Boot_Select&DataDump;
  INA219 peak-catcher catches 8 mA / 15 ms flash-write bursts that
  the 68 ms averaging window hid.
- 2026-04-01 / 02: tier4 v1-v5 (v2 broken by a partial SAADC refactor,
  rejected).
- 2026-04-05: tier4_with_maintenance_branching stable, ~70 uA; USB CDC
  ACM dump, CSV verified; stability fixes: FP removal, no over-forcing
  of HFCLK/SAADC.
- 2026-04-06: mess_tier4; hardened records v1.
- 2026-04-07: hardened records v2: 32-byte CRC records, time anchors,
  RTC hooks.
- 2026-04-08 / 09: QSPI bring-up regression root-caused (IO2/IO3 =
  WP#/HOLD# must be high; hardware QSPI block deactivated); the
  AI-hardened logger had regressed a proven transport; a flash_errors
  RAM counter gave false confidence; 128-event dump; 18,900 records
  in 2 h; 40 s boot energy spike identified.
- 2026-04-10: page_batch_chkpt_resume: 32,640 records in one run; the
  boot spike is eliminated by design.
- 2026-04-13: sparse events + checkpoint16; batching vs direct-write
  decision recorded.
- 2026-04-14: canonical 13-field contract decided (vertical slice).
- 2026-04-16: canonical export v2: 1,459 records across boots with
  boot_id.

## 2026-04 to 06: LoRaWAN end-to-end

- 2026-04-29: gateway bring-up (Pi 400 + RAK5146); TTN device created
  2026-04-29 21:07:04, US915 FSB2.
- 2026-05-03: nb1 RUI3 mobile beacon; coverage-probe procedure.
- 2026-05-10: bridge dummy pass.
- 2026-05-11: Zephyr-to-RAK control loop + FSM; TX_DONE parser race
  fixed; knock wake from 1.7 uA AT+SLEEP.
- 2026-05-19: FSM untethered final run (MQTT catcher, fsync).
- 2026-06-08 / 06-26: last TTN activity (bench sessions end).

## 2026-09 to 11: wilda repo, nRF54 era, assembly

- 2026-09-06: wilda repo created (nRF54L15 era begins).
- 2026-09-13: blinky working on nRF54L15-TAG, blue LED P2.09.
- 2026-09-23: IMU read, CSV output verified; negative value bug fixed.
- 2026-10-08: BLE wireless IMU streaming, 4G / 2000 dps.
- 2026-11-12: nRF52/RAK legacy archive committed as evidence.
- 2026-11-14: credential scrub (see note above), README v0, timeline.
