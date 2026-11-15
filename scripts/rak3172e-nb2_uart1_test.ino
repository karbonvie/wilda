void setup() {
    // Keep UART2 active for PC USB debugging and future flashing
    Serial.begin(115200, RAK_AT_MODE); 
    
    // Enable UART1 AT Parser for the Adafruit Feather
    Serial1.begin(115200, RAK_AT_MODE); 
}

void loop() {
    // Kill the Arduino loop thread immediately to drop the STM32WL into low power mode
    api.system.scheduler.task.destroy();
}
