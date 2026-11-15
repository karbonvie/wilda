# Bench scripts

Bench utilities, not deliverables. Kept because they back the
measurement claims and the LoRaWAN validation runs.

- rak3172e_nb1_beacon_v3.ino : RUI3 mobile beacon for the RAK3172
  evaluation board. Button clicks select 30 s / 60 s / 10 min
  uplink interval, US915 FSB2, unconfirmed uplinks. Final of three
  iterations (v1/v2 retained only in local archives: v3 fixes the
  LED pin to PA0 and restores the blink confirmation).
- catcher.py : MQTT catcher bench utility. Subscribes to a TTN
  application uplink topic, writes decoded fields to CSV with
  fsync. Credentials redacted; paste your own before use.
- wildlife_data_sample.csv : first rows of the 2026-05-19
  validation run (Tag=1, Evt=1, S1=1000 mV, S2=2000 mV).
- getcurrent_ina219_kb_v3.ino : INA219 peak-catcher instrument.
  532 us conversion window, catches the 15 ms / 8 mA flash-write
  bursts that the 68 ms averaging window hides (see
  docs/decisions/ADR-005).
- pulsegen_v09.ino : UNO R4 pulse generator, PWM and one-shot
  modes, serial controlled. The event source for AIN pulse
  validation (240 mV/pulse on the verified node v3).
- rak3172e-nb2_uart1_test.ino : minimal RUI3 UART1 AT-mode
  bring-up sketch for the Zephyr-to-modem bridge.
- ttn_payload_decoder.js : TTN uplink payload formatter, verbatim from the TTN console. Maps the 8-byte record payload (v, tag_id, event_type, flags, sensor1_mv, sensor2_mv) to the fields catcher.py writes to CSV.
