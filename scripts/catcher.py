import paho.mqtt.client as mqtt
import json
import os
import csv
from datetime import datetime

# --- CONFIGURATION ---
TTN_BROKER = "nam1.cloud.thethings.network"
TTN_PORT = 8883
# TTN MQTT Username requires the @ttn suffix
TTN_USERNAME = "your-app-id@ttn" 
TTN_PASSWORD = "REDACTED: paste your own NNSXS key here"
# The '+' is an MQTT wildcard that listens to all devices in the application
TTN_TOPIC = "v3/your-app-id@ttn/devices/+/up"
CSV_FILE = "wildlife_data.csv"

# --- CSV INITIALIZATION ---
# Write the header if the file does not exist
if not os.path.exists(CSV_FILE):
    with open(CSV_FILE, mode='w') as f:
        f.write("local_time,device_id,v,tag_id,event_type,flags,sensor1_mv,sensor2_mv\n")

# --- MQTT CALLBACKS ---
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print("[SYS] Connected to TTN MQTT Broker successfully.")
        client.subscribe(TTN_TOPIC)
        print(f"[SYS] Subscribed to topic: {TTN_TOPIC}")
        print("[SYS] Waiting for telemetry data...")
    else:
        print(f"[ERR] Failed to connect, return code {rc}")

def on_message(client, userdata, msg):
    print(f"\n[SYS] Raw packet detected on topic: {msg.topic}")
    try:
        payload = json.loads(msg.payload.decode('utf-8'))
        dev_id = payload.get("end_device_ids", {}).get("device_id", "unknown")

        # Check if the TTN Javascript formatter successfully parsed it
        uplink = payload.get("uplink_message", {})
        decoded = uplink.get("decoded_payload")

        if decoded:
            local_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            v = decoded.get("v", "")
            tag_id = decoded.get("tag_id", "")
            event_type = decoded.get("event_type", "")
            flags = decoded.get("flags", "")
            s1_mv = decoded.get("sensor1_mv", "")
            s2_mv = decoded.get("sensor2_mv", "")

            print(f"[{local_time}] Rx from {dev_id}: Tag={tag_id}, Evt={event_type}, S1={s1_mv}mV, S2={s2_mv}mV")

            with open(CSV_FILE, mode='a', newline='') as f:
                writer = csv.writer(f)
                writer.writerow([local_time, dev_id, v, tag_id, event_type, flags, s1_mv, s2_mv])
                f.flush()
                os.fsync(f.fileno())
        else:
            print(f"[WARN] Packet from {dev_id} received, but 'decoded_payload' is missing.")
            print("[WARN] Did you save the Javascript Payload Formatter in TTN?")

    except Exception as e:
        print(f"[ERR] Failed to process message: {e}")


# --- MAIN EXECUTION ---
client = mqtt.Client()
client.username_pw_set(TTN_USERNAME, TTN_PASSWORD)
client.on_connect = on_connect
client.on_message = on_message

# Enable TLS/SSL for secure connection to TTN
client.tls_set()

print("[SYS] Connecting to The Things Network...")
try:
    client.connect(TTN_BROKER, TTN_PORT, 60)
    client.loop_forever()
except KeyboardInterrupt:
    print("\n[SYS] Shutting down Catcher Script.")
    client.disconnect()
