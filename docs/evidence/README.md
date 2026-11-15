# Evidence

Screenshots and bench photos backing the milestone claims. One line
per file. File names carry the capture date. Images are added in a
separate commit after blurring.

## Files and captions

- 2026-04-29_ttn-console.png : TTN console, both bench devices
  visible, last activity June 2026.
- 2026-04-29_ttn-device-page.png : device page, created 2026-04-29
  21:07:04, US915 FSB2.
- 2026-05-03_beacon-arduino.png : Arduino IDE with the RUI3 mobile
  beacon sketch, rak3172e_nb1_beacon_v3.ino.
- 2026-05-03_beacon-serial.png : Serial Monitor, Mobile Beacon Mode 1
  at 30 s intervals, +EVT:TX_DONE uplinks.
- 2026-05-11_fsm-serial-log.png : FSM serial log, UPLINK COMPLETE,
  sleep and knock-wake cycle.
- 2026-05-11_ttn-live-data.png : live uplink, payload
  01 01 01 00 03 E8 07 D0, SF7BW125, RSSI -55 dBm, SNR 13.5.
- 2026-05-19_catcher-output.png : catcher.py terminal, 2026-05-19
  20:38, Tag=1 Evt=1 S1=1000 mV S2=2000 mV.
- 2026-03-19_current-breakdown.png : text capture of the nRF52840
  active-current breakdown, about 20-25 mA (36 mA whole-DK active).
- dk-bench-1.png, dk-bench-2.png, dk-bench-3.png : nRF52 DK bench,
  event_wake era, jumper wires and J-Link attached.

## Mandatory blur and check list, before adding any image

1. Every TTN screenshot: draw a solid rectangle over the
   application name (top left of the console) and over the gateway
   id (gateway list). Device names stay visible.
2. Any screenshot showing source code or terminal commands: check
   for hex strings of 16 or 32 characters (device keys), blur them.
3. Bench photos: nothing to blur; the J-Link serial number on the
   probe is harmless and stays.
Method: any image editor, filled black rectangle, export as PNG.
