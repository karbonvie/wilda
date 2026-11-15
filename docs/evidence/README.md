# Evidence

Screenshots and bench photos backing the milestone claims. One line
per file. File names carry the capture date. Images are added in a
separate commit after redaction.

## Files and captions

- 2025-10-31_traces-1.png, -2.png, -3.png : AD2 scope traces of
  the event_wake pulse inputs, timestamps 2025-10-31 in the
  captures, same day as the all_ok_final snapshot.
- 2026-03-19_current-breakdown.png : text capture of the nRF52840
  active-current breakdown, about 20-25 mA (36 mA whole-DK active).
- 2026-04-23_ttn-gateway-page.png : TTN gateway overview, US915
  FSB2, created 2026-04-23 21:32:43, connection statistics.
- 2026-04-29_ttn-console.png : TTN application overview, both
  bench devices, uplink logs.
- 2026-04-29_ttn-device-page.png : device page, created 2026-04-29
  21:07:04, US915 FSB2.
- 2026-05-03_beacon-arduino.png : Arduino IDE with the RUI3 mobile
  beacon sketch, rak3172e_nb1_beacon_v3.ino.
- 2026-05-03_beacon-serial.png : Serial Monitor, Mobile Beacon
  Mode 1 at 30 s intervals, +EVT:TX_DONE uplinks.
- 2026-05-11_fsm-serial-log.png : FSM serial log, UPLINK COMPLETE,
  sleep and knock-wake cycle.
- 2026-05-11_ttn-live-data.png : live uplink, payload
  01 01 01 00 03 E8 07 D0, SF7BW125, RSSI -55 dBm, SNR 13.5.
- 2026-05-19_catcher-output.png : catcher.py terminal, 2026-05-19
  20:38, Tag=1 Evt=1 S1=1000 mV S2=2000 mV.
- dk-bench-1.png, dk-bench-2.png : DK bench photos, event_wake-era
  setup with jumper wires and debug probe attached.

## Mandatory redaction checklist, before adding any image

1. Every TTN screenshot: solid rectangle over the application name
   and the application id, and over the gateway name, gateway id
   and gateway EUI. End device names stay visible.
2. Uplink log lines showing a DevAddr (8 hex bytes): solid
   rectangle over every occurrence.
3. Terminal screenshots showing MQTT topic lines (v3/...@ttn/...):
   solid rectangle over the topic text.
4. Any screenshot showing source code: check for 16 or 32 character
   hex strings, cover them.
5. Bench photos: nothing to cover; the probe serial number on the
   debugger is harmless and stays.
Method: solid black rectangles only, never blur or mosaic, export
as PNG.
