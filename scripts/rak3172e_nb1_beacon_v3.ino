// --- BEGIN RESTORED FIRMWARE ---

uint32_t tx_interval_ms = 30000; // Default interval set to 30 seconds
uint32_t last_tx_time = 0;       // Tracks the timestamp of the last transmission

// The 8-byte dummy payload expected by the TTN decoder
uint8_t payload[] = {0x01, 0x01, 0x01, 0x00, 0x03, 0xE8, 0x07, 0xD0};

// Hardware pin definitions for the evaluation board
#define BTN_PIN PB5 
#define LED_PIN PA0 // Note: Set to PA0 for the standard Green LED on RAK3172-E

// State variables for the physical button interrupt logic
volatile uint8_t click_count = 0; // Volatile memory prevents optimization out of the ISR
uint32_t window_start_time = 0;   // Timestamp for the 2-second click listening window
bool is_counting = false;         // Flag indicating if we are currently accepting clicks

// Network event callback executed by the RUI3 MAC layer
void joinCallback(int32_t status) {
    if (status != 0) {
        // The RUI3 auto-join background task manages retries automatically.
        // We log the status and yield back to the system.
        Serial.printf("Join pending (Status: %d). Auto-retry scheduled...\n", status);
    } else {
        Serial.println("Joined TTN! Network session active.");
    }
}

// Hardware Interrupt Service Routine for the push-button
void button_isr() {
    static uint32_t last_irq = 0; 
    // 250ms software debounce filters out electrical noise from the mechanical switch
    if (millis() - last_irq > 250) { 
        click_count++;
        last_irq = millis();
        
        // If this is the first click, open the 2-second window to count subsequent clicks
        if (!is_counting) {
            is_counting = true;
            window_start_time = millis();
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000); // Allow USB to enumerate for serial monitoring
    Serial.println("--- RAK3172 RUI3 Mobile Beacon ---");
    
    // Configure hardware pins
    pinMode(LED_PIN, OUTPUT);
    pinMode(BTN_PIN, INPUT_PULLUP); // Utilize internal pull-up resistor for the button
    
    // Attach the interrupt to trigger when the button connects PB5 to Ground
    attachInterrupt(BTN_PIN, button_isr, FALLING);

    // Apply physical RF region and sub-band constraints
    api.lorawan.band.set(RAK_REGION_US915);
    uint16_t mask[4] = {0x0002, 0, 0, 0}; // Restrict to TTN FSB 2 (Channels 8-15)
    api.lorawan.mask.set(mask);
    
    // Apply LoRaWAN MAC parameters
    api.lorawan.njm.set(1); // Mode 1 corresponds to OTAA
    api.lorawan.dr.set(3);  // Data Rate 3 corresponds to SF7 (242 byte payload capacity)
    api.lorawan.adr.set(0); // Disable Adaptive Data Rate to prevent channel mask modification
    api.lorawan.cfm.set(0); // Unconfirmed uplinks to respect TTN downlink duty cycle limits

    // Link our custom callback function to the network join event
    api.lorawan.registerJoinCallback(joinCallback);

    Serial.println("Initiating explicit Auto-Join (1:1:10:8)...");
    // Explicit parameters matching AT+JOIN=1:1:10:8
    // Format: join(join_cmd, auto_join, auto_join_period, auto_join_cnt)
    api.lorawan.join(1, 1, 10, 8); 
}

void loop() {
    // 1. Process Button Clicks (Executes only after the 2-second window closes)
    if (is_counting && (millis() - window_start_time > 2000)) { 
        is_counting = false;
        
        // Map the number of clicks to specific transmission intervals
        if (click_count == 1) {
            tx_interval_ms = 30000;
            Serial.println("Mode 1: 30s");
        } else if (click_count == 2) {
            tx_interval_ms = 60000;
            Serial.println("Mode 2: 60s");
        } else if (click_count >= 3) {
            click_count = 3; // Maximum visual blinks
            tx_interval_ms = 600000;
            Serial.println("Mode 3: 10m");
        }

        Serial.printf("Confirmed %d clicks. Interval updated.\n", click_count);

        // RESTORED: Blink the LED to visually confirm the selection
        for (int i = 0; i < click_count; i++) { 
            digitalWrite(LED_PIN, HIGH);  // Turn LED on
            delay(200);                   // Wait
            digitalWrite(LED_PIN, LOW);   // Turn LED off
            delay(200);                   // Wait
        }

        click_count = 0; // Reset counter for the next user interaction
    }

    // 2. Transmit Beacon Payload
    // Check if the Network Join Status equals 1 (Active Session)
    if (api.lorawan.njs.get() == 1) {
        // Evaluate the non-blocking timer
        if (millis() - last_tx_time >= tx_interval_ms) {
            last_tx_time = millis();
            Serial.print("Transmitting... ");
            
            // RESTORED: Flash LED quickly to show transmission
            digitalWrite(LED_PIN, HIGH); 

            // Format: send(length, payload_pointer, fport, confirmed)
            if (api.lorawan.send(sizeof(payload), payload, 2, false)) { 
                Serial.println("Queued to MAC.");
            } else {
                Serial.println("MAC Busy. Packet dropped.");
            }

            // RESTORED: Turn LED off after queueing
            digitalWrite(LED_PIN, LOW); 
        }
    }
    
    // Short delay yields execution back to the RTOS for background MAC processing
    delay(10); 
}

// --- END RESTORED FIRMWARE ---