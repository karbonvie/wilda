/*
--- README.md for wilda project ---
--- place this file at: ~/wilda/README.md ---
--- human-facing project summary for GitHub visitors, researchers, collaborators ---

Version 20260913_1
--- 1: Overview ---

WildlifeThrive (wilda) is an open-source, ultra-low-power wildlife telemetry tag project dedicated in honor of Sir David Attenborough. The goal is to create affordable, customizable tracking hardware that researchers can deploy without vendor lock-in.

Traditional wildlife tracking tags are expensive, closed-source, power-hungry, and inaccessible to many researchers. wilda builds open-source alternatives using the Nordic nRF54L15 SoC, TinyML on-device behavior classification, LoRaWAN telemetry via TTN, and BLE Channel Sounding for proximity detection.


--- 2: Target Applications ---

- Avian tracking: sub-5g tags for birds (peregrine falcons, songbirds)
- Small mammal monitoring: ultra-light collars for squirrels, bats (4g target)
- Marine conservation: waterproof tags for turtles, aquatic species
- Behavioral studies: TinyML-powered classification of movement patterns
- Environmental sensing: static nodes for habitat monitoring


--- 3: Key Hardware ---

Primary tag platform: Nordic nRF54L15-TAG
  - Cortex-M33 at 128MHz + RISC-V co-processor
  - 256KB RAM, 1.5MB flash
  - BLE 6.0 Channel Sounding (sub-meter proximity)
  - Onboard sensors: BME688 (environmental), BMI270 (6-axis IMU), ADXL367 (low-power accelerometer)
  - 0.7uA sleep current, 50% radio current reduction vs nRF52
  - CR2032 coin cell powered

LoRaWAN modem: RAK3172 (STM32WLE5, 915MHz US/CAN)
LoRaWAN gateway: RAK5146 PiHAT on Raspberry Pi 5

Backend: TTN MQTT -> catcher.py (Python) -> CSV with fsync -> InfluxDB -> Grafana


--- 4: Development Timeline ---

Phase 1 (2026 Q1-Q2): nRF52840 baseline
  Achieved 50-70 uA average current, QSPI flash logging, 32-byte hardened event format, sparse-event storage model.

Phase 2 (2026 Q2): LoRaWAN integration
  RAK3172 AT command driver, OTAA join with TTN, MQTT catcher backend, stage 3 unification (QSPI logging + LoRaWAN in single image).

Phase 3 (2026 Q2-Q3): Strategic pivot to nRF54L15
  Abandoned nRF52 due to TinyML limitations. nRF54L15 is purpose-built for edge AI with native Nordic Edge AI Studio / Neuton integration.

Current state: nRF54L15-TAG in hand, fresh firmware development starting from microproject 01-blinky.


--- 5: Project Structure ---

  firmware/         - nRF54L15 tag firmware (production target)
  microprojects/    - isolated experiments (01-blinky through 08-end-to-end)
  backend/          - TTN payload decoders, MQTT catcher (catcher.py), data
  hardware/         - enclosure, test fixture, BOM
  tools/            - power analysis, flash dump parser, signal gen scripts
  docs/             - decisions, hardware notes, meetings, references
  archive/          - nRF52 legacy code, tagged releases


--- 6: TinyML Pipeline ---

1. Collect accelerometer data at 50Hz (1000+ samples per behavior class)
2. Train model in Nordic Edge AI Studio using Neuton
3. Target: nRF54L15, TinyML preset, model size <80KB
4. Export as C library, integrate into nRF Connect SDK
5. On-device inference: buffer 50 samples (1 second), classify behavior
6. Transmit classification result via LoRaWAN only for significant events
7. Target: <100uA average current for 1 classification/hour


--- 7: Data Path ---

Tag (nRF54L15) -> UART -> RAK3172 -> LoRaWAN US915 FSB2 -> TTN -> MQTT -> catcher.py -> CSV (fsync) -> InfluxDB -> Grafana

Payload format: 8-byte binary (version, tag_id, event_type, flags, behavior_class, confidence, sensor_data)


--- 8: Open Source ---

Firmware: MIT license
Hardware: CERN OHL (planned)
Documentation: CC BY-SA

GitHub organization: WildlifeThrive
  - wildlifethrive-firmware
  - wildlifethrive-hardware
  - wildlifethrive-docs


--- 9: Collaboration ---

We welcome collaboration with researchers, field practitioners, and open-source contributors. Current contacts include Universite Laval, Universite de Sherbrooke, Ocean Tracking Network, Acadia University, Birds Canada (Motus), Western University, Arribada Initiative, Smart Parks/OpenCollar, and WILDLABS/WWF.

What researchers need:
  - Mass limits: <=4g for small mammals, <=5g for birds
  - Deployment life: 6-12 months on single charge
  - Data schema: Movebank-compatible, no manual cleanup
  - Cost: open-source, <$50 per tag at scale
  - Reliability: field-proven, fail-safe local logging


--- 10: Reference Projects ---

TickTag: https://github.com/trichl/TickTagOpenSource
SnapperGPS: https://snappergps.info/
LocoMote: https://pmc.ncbi.nlm.nih.gov/articles/PMC11615471/


--- 11: Status ---

2026-09-06: Project structure created
2026-09-13: agents.md created, hardware research complete, development environment defined
Current focus: microproject 01-blinky (verify build/flash/debug on nRF54L15-TAG)


--- 12: Repository Navigation ---

For full technical context, AI agent instructions, hardware pinout details, and development workflow, see agents.md in the repository root.

For architecture decisions, see docs/decisions/.
For hardware notes and pinouts, see docs/hardware/.
For weekly status and session reports, see docs/meetings/. Each session report ends with an Operations Guide section listing all commands, tool steps, and physical board actions needed to reproduce the session, so work can resume after a 1-2 week break without re-deriving the workflow.
For reference papers and datasheets, see docs/references/.
*/
