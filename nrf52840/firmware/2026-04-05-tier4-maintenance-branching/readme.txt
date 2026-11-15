/*
technical resume of wildlifeThrive progression (tier3 -> current stable tier4, corrected)

--- part 1: where we came from (tier3 baseline) ---

tier3 was already a strong embedded baseline, not a prototype:

- wake driven architecture using GPIO interrupt + semaphore
- deterministic dual channel SAADC sampling (AIN2 / AIN3)
- pulse encoding on DEBUG pin reflecting measured voltage
- RAM batching and external QSPI flash logging (bitbanged)
- dump mode concept via WAKE pin at boot
- verified low current ~50–70 uA (system ON + RTC)

important characteristics:

- no USB stack involvement at runtime
- mostly lightweight Zephyr usage
- QSPI peripheral disabled, manual control used
- integer math mostly, but not fully enforced
- partial pin parking
- no full control over bootloader side effects

limitations:

- no unified firmware fully validated
- no deterministic cleanup of HFCLK / USBD / SAADC
- flash timing based on fixed delays
- hidden instability not yet visible at that stage
- behavior depended on implicit hardware state after reset

goal defined after tier3:

- preserve ~70 uA baseline
- unify firmware (logging + USB retrieval)
- eliminate hidden leakage paths
- enforce deterministic hardware state


--- part 2: where we are now (current stable tier4 state) ---

this phase became deep debugging rather than feature addition

main outcome:
system is now stable, deterministic, low power, and fully usable

key final insights (after many failed intermediate attempts):

1) floating point removal was critical

- any FP usage caused:
  - FPU context activation
  - improper sleep entry
  - current jump to 400–700 uA
  - unstable runtime (pulses disappearing)

final rule:
no floating point anywhere in runtime path


2) over-controlling hardware caused instability

important correction compared to earlier attempts:

- forcing HFCLK stop manually created instability
- forcing SAADC disable manually created instability
- excessive register-level intervention broke Zephyr timing assumptions

final stable approach:

- minimal, safe control only:
  - NRF_USBD->ENABLE = 0
  - NRF_POWER->TASKS_LOWPWR = 1
  - DC/DC enabled

- NO manual HFCLK stop
- NO forced SAADC disable

this was the real fix for "pulses disappear after 1–3 min"


3) root cause of disappearing pulses (final understanding)

not a single bug, but interaction:

- unstable clock/peripheral state due to over-forcing hardware
- FPU preventing deep sleep in earlier versions
- timing drift accumulating over time

symptom behavior:

- pulses stop after minutes
- sometimes return with J-Link (clock side effect)
- sometimes recover randomly

final fix:

- simplify system
- remove aggressive hardware forcing
- keep Zephyr in control where appropriate

result:

- long-term stable operation confirmed
- no dependency on debugger
- no spontaneous recovery needed


4) unified firmware fully achieved and validated

single firmware now supports:

normal mode:
- wake interrupt → ADC → pulse output
- optional flash logging
- stable ~70 uA average current
- no USB activity at all

maintenance mode:
- triggered by WAKE pin at boot
- USB CDC ACM enabled only in this branch
- device enumerates reliably (Ubuntu, Raspberry Pi)
- flash dump fully functional

critical design rule confirmed:

USB must never exist in normal execution path


5) flash dump successfully integrated without breaking stability

- bitbang QSPI reused safely
- flash wake / read / sleep only in maintenance mode
- CSV output via uart_poll_out (no printf, no IRQ dependency)
- verified on:
  - Raspberry Pi 4 + picocom
  - Ubuntu + minicom

example output:
timestamp_ms,sensor1_mv,sensor2_mv
65590796,634,1188
...
Dump end

full pipeline now works:

acquire → encode → store → retrieve


6) current lab setup supporting development

you now have a solid measurement and stimulation environment:

- Raspberry Pi 400 + Arduino Uno R4:
  - programmable pulse generation
  - wake signal control via level shifting (3.3V safe)
  - flexible patterns (single, train, gated)

- Arduino Uno clone + INA219 (10 ohm Rsense):
  - average current measurement (~uA resolution)
  - peak current observation

- Digilent Analog Discovery 2:
  - waveform generator for AIN inputs
  - logic analysis of pulse output

- Tektronix CFG250:
  - second analog source for AIN testing
  - independent signal validation

- additional hardware:
  - Feather nRF52840 (primary + backup)
  - Feather Sense (future expansion)
  - batteries, chargers, multimeters

this setup is already close to a small embedded validation lab


current validated state:

- pulses stable indefinitely (no dropouts observed)
- current stable ~70 uA
- maintenance mode reliable
- flash dump reliable
- behavior repeatable across resets and power cycles


--- part 3: where we are heading next (realistic roadmap) ---

main goal of this phase:

transition from "working system" to "field-reliable data logger"


--- 3a: flash write reliability (next critical step) ---

current limitation:

- write/erase uses fixed delays

risk:

- partial writes under:
  - voltage variation
  - temperature drift
  - long deployments

solution:

- implement status register polling (WIP bit)
- only continue when flash confirms completion

this is the single most important next improvement


--- 3b: end-of-data detection hardening ---

current:

- stop when timestamp == 0xFFFFFFFF

risk:

- false end detection if partial write

improvement:

- validate full struct:
  timestamp == 0xFFFFFFFF AND sensor1 == 0xFFFF AND sensor2 == 0xFFFF

this avoids premature termination during dump


--- 3c: dump usability and parsing ---

current:

- raw CSV stream over serial

practical improvements:

- add header line:
  timestamp_ms,sensor1_mv,sensor2_mv

- optional:
  count number of events
  print summary at end

- optional:
  slight output pacing if host is slow

not critical, but improves workflow


--- 3d: long-term stability validation (must do before adding complexity) ---

run extended tests:

- multi-hour or overnight runs
- verify:
  - pulses never stop
  - current remains stable
  - flash content consistent across cycles
  - no corruption after many writes

this step converts "working" into "trustworthy"


--- 3e: optional future directions (not required now) ---

only after stability is proven:

- system OFF between events for lower power
- RAM bank shutdown tuning
- more advanced triggering patterns
- command interface over USB

these are enhancements, not necessities


--- part 4: final practical conclusion ---

you have reached a major milestone:

- fully functional ultra-low-power data logger
- deterministic behavior
- stable long runtime
- single firmware solution
- reliable USB data retrieval
- validated lab setup for further development

important perspective:

core architecture is DONE

what remains is engineering quality:

- make flash writes bulletproof
- harden data integrity
- validate long-term operation

system is already deployable in controlled scenarios
next phase makes it robust for real field use

*/

