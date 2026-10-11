/*
--- README.md for wilda project ---
--- place this file at: ~/wilda/README.md ---
--- human-facing project summary for GitHub visitors, researchers, collaborators ---

Version 20261011_1
--- 1: Overview ---

WildlifeThrive (wilda) is an open-source, ultra-low-power wildlife telemetry tag project dedicated in honor of Sir David Attenborough. The goal is to create affordable, customizable tracking hardware that researchers can deploy without vendor lock-in.

Traditional wildlife tracking tags are expensive, closed-source, power-hungry, and inaccessible to many researchers. wilda builds open-source alternatives using the Nordic nRF54LM20A SoC, TinyML on-device behavior classification, LoRaWAN telemetry via TTN, and BLE Channel Sounding for proximity detection.


--- 2: Target Applications ---

Primary research targets:
- Avian tracking: sub-5g tags for birds (peregrine falcons, songbirds)
- Bat monitoring: ultra-light tags for bats (4g target, high sample rate)
- Marine conservation: waterproof tags for turtles, aquatic species
- Behavioral studies: TinyML-powered classification of movement patterns
- Environmental sensing: static nodes for habitat monitoring

Training proxy animals (accessible, not research targets):
- Dog (own) and cat (friend's): used as proxy for wild mammal behavior
  classification. Readily available for physical tagging with permissions.
  The pet model proves the TinyML pipeline; wild species models use
  existing published datasets (no permits to tag wild animals).


--- 3: Key Hardware ---

Primary platform: Seeed Studio XIAO nRF54LM20A Sense
  - nRF54LM20A SoC: Cortex-M33 at 128MHz + RISC-V coprocessor
  - 512 KB RAM, 2 MB NVM (RRAM), 8 MB external SPI flash
  - BLE 6.0 Channel Sounding, Thread, Zigbee, Matter
  - Onboard IMU: LSM6DS3TR-C (6-axis, I2C)
  - Onboard microphone: MSM261DGT006 (PDM)
  - nPM1300 PMIC: battery charging, fuel gauge, ship mode (0.33 uA)
  - USB-C: flashing, debugging, data download (SAMD11 CMSIS-DAP)
  - Deep sleep: 4.76 uA
  - Size: 21 x 17.8 mm (compact enough for dog collar or small tag)
  - Global RTC: runs in System OFF (0.8 uA), provides wall-clock timestamps

LoRaWAN modem: RAK3172 (STM32WLE5, 915MHz US/CAN)
LoRaWAN gateway: RAK5146 PiHAT on Raspberry Pi 5

Backend: TTN -> MQTT -> catcher.py (Python) -> CSV -> InfluxDB -> Grafana


--- 4: Development Timeline ---

Phase 1 (2026 Q1-Q2): nRF52840 baseline
  Achieved 50-70 uA average current, QSPI flash logging, 32-byte hardened event format, sparse-event storage, LoRaWAN integration with TTN.

Phase 2 (2026 Q3): nRF54L15-TAG exploration (archived)
  Built and verified blinky, IMU reads (BMI270 via SPI), BLE wireless IMU streaming. Collected human walk/run/stationary data at 4G/2000dps. Archived all microprojects to archive/nrf54l15-tag-microprojects/.

Phase 3 (2026 Q4, current): nRF54LM20A single-SoC strategy
  Strategic pivot to nRF54LM20A. Key enabler: 8 MB external flash on Seeed XIAO nRF54LM20A Sense provides 4.4 hours of IMU logging at 50 Hz (binary int16), eliminating BLE dropout problem for outdoor data collection. 512 KB RAM doubles TinyML model capacity. nPM1300 PMIC provides ship mode and battery charging. Hardware not yet ordered.


--- 5: Project Structure ---

  firmware/nrf54lm20a/  - main tag firmware (production target, not yet started)
  microprojects/        - isolated experiments for nRF54LM20A
    01-blinky/          - build/flash/debug on XIAO [not started]
    02-imu-read/        - LSM6DS3TR-C via I2C [not started]
    03-imu-ble/         - BLE IMU streaming [not started]
    04-flash-log/       - log to 8 MB external flash [not started]
    05-data-download/   - USB-C data retrieval [not started]
    06-sleep-current/   - deep sleep / ship mode measurement [not started]
    07-pet-data/        - dog+cat behavior data (proxy for wild mammals) [not started]
    08-wild-data/       - survey/acquire existing datasets for birds, bats, turtles [not started]
    09-tinyml-train/    - multi-species parallel model training [not started]
    10-tinyml-deploy/   - on-device inference, species-specific models [not started]
    11-lora-uart/       - RAK3172 communication [not started]
    12-lorawan-join/    - TTN join [not started]
    13-end-to-end/      - full pipeline [not started]
  backend/              - TTN decoders, MQTT catcher, data
  hardware/             - enclosure, test fixture, BOM
  tools/                - power analysis, flash dump parser, signal gen
  docs/                 - decisions, hardware, meetings, references
  archive/              - nRF52 legacy, nRF54L15-TAG microprojects, tagged releases


--- 6: TinyML Pipeline ---

1. Log IMU data to 8 MB external flash (binary int16, 12 bytes/sample, 50 Hz)
2. Download via USB-C, convert binary to CSV
3. Train model in Nordic Edge AI Studio using Neuton
4. Target: nRF54LM20A, TinyML preset, model size <80 KB
5. Export as C library, integrate into nRF Connect SDK
6. On-device inference: buffer 50 samples (1 second), classify behavior
7. Transmit classification via LoRaWAN only for significant events
8. Target: <100 uA average current for 1 classification/hour

Multi-species training strategy:
  Track A (pet proxy): Dog + cat data collected via physical tagging. Triple-tap sync, one-behavior-per-clip (30-60 sec). Dog classes: walk, trot, run, stationary, shake, jump. Cat classes: walk, run, stationary, jump/climb, groom, play. Sensor: 2G/500dps, 50 Hz. Proves the pipeline, baseline for wild mammal transfer.
  Track B-D (wild species): Existing published datasets for birds (flight, foraging, perching), bats (flight, foraging, roosting), turtles (swimming, diving, resting). No physical tagging of wild animals (no permits). Data from Movebank, Zenodo, ICARUS, published papers.


--- 7: Data Path ---

Tag (nRF54LM20A) -> UART -> RAK3172 -> LoRaWAN US915 FSB2 -> TTN -> MQTT -> catcher.py -> CSV (fsync) -> InfluxDB -> Grafana

Local logging: IMU data -> 8 MB external flash -> USB-C download -> binary-to-CSV -> Edge AI Studio

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

2026-09-13: 01-blinky done on nRF54L15-TAG (archived)
2026-09-22: 02-imu-read CSV output verified on nRF54L15-TAG (archived)
2026-10-03: 02-imu-read BTN1 System OFF + LED heartbeat on nRF54L15-TAG (archived)
2026-10-08: 02-imu-read-part2-ble done on nRF54L15-TAG (archived)
2026-10-08: Strategic pivot to nRF54LM20A. All nRF54L15-TAG microprojects archived. agents.md and README.md rewritten for LM20A-only strategy.
2026-10-11: Multi-species TinyML strategy. Dogs/cats are training proxies for wild mammals. Main targets: birds, bats, turtles (existing datasets, no physical tagging). Microproject roadmap expanded to 13 folders.
Current focus: Planning platform bring-up. Seeed XIAO nRF54LM20A Sense not yet ordered.


--- 12: Repository Navigation ---

For full technical context, AI agent instructions, hardware pinout details, and development workflow, see agents.md in the repository root.

For architecture decisions, see docs/decisions/.
For hardware notes and pinouts, see docs/hardware/.
For weekly status and session reports, see docs/meetings/. Each session report ends with an Operations Guide section listing all commands, tool steps, and physical board actions needed to reproduce the session.
For reference papers and datasheets, see docs/references/.
For nRF54L15-TAG era microprojects, see archive/nrf54l15-tag-microprojects/.
For nRF52 era code, see archive/nrf52-legacy/.
*/
