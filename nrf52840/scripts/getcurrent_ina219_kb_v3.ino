// --------------------------------------------------------------------
// INA219 Fast Peak-Catcher + Voltage (Arduino)
// --------------------------------------------------------------------
#include <Wire.h>
#include <Adafruit_INA219.h>

Adafruit_INA219 ina219;

#define RSENSE_PHYSICAL 10.0
#define INA219_DEFAULT_RSENSE 0.1
const float current_scale = INA219_DEFAULT_RSENSE / RSENSE_PHYSICAL;

void setup(void) {
  Serial.begin(115200);
  while (!Serial) { delay(1); } 
    
  if (!ina219.begin()) {
    Serial.println("Error: INA219 not found");
    while (1) { delay(10); }
  }

  // Set calibration to 400mA range (highest gain/resolution for uA)
  ina219.setCalibration_16V_400mA();

  // Override Configuration Register (0x00) to 0x041F:
  // - 16V Bus Range
  // - 40mV Shunt Gain
  // - 12-bit ADC (532us conversion time)
  // - Continuous Mode
  Wire.beginTransmission(0x40);
  Wire.write(0x00);
  Wire.write(0x04);
  Wire.write(0x1F);
  Wire.endTransmission();

  Serial.println("Monitoring: Avg uA, Peak uA, and Bus Voltage");
}

void loop(void) {
  float peak_uA = 0;
  float sum_uA = 0;
  float sum_V = 0;
  int samples = 0;
  
  uint32_t t_start = millis();
  
  // Tight loop: Sample for 1000ms to catch the 15ms Flash blast
  while (millis() - t_start < 1000) {
    // getCurrent_mA() reads the Shunt Voltage register
    float current_uA = ina219.getCurrent_mA() * current_scale * 1000.0;
    
    // Capture the absolute highest spike seen in this 1-second window
    if (current_uA > peak_uA) {
      peak_uA = current_uA;
    }
    
    sum_uA += current_uA;
    
    // Periodically sample bus voltage (less critical than current spikes)
    if (samples % 10 == 0) {
        sum_V += ina219.getBusVoltage_V();
    }
    
    samples++;
    
    // No delay() here. We want to poll as fast as the I2C bus allows
    // to ensure we don't miss the 15ms Flash write event.
  }

  float avg_uA = sum_uA / samples;
  float avg_V  = sum_V / (samples / 10);

  Serial.print("Bus: "); Serial.print(avg_V, 3); Serial.print("V | ");
  Serial.print("Avg: "); Serial.print(avg_uA, 1); Serial.print(" uA | ");
  Serial.print("PEAK: "); Serial.print(peak_uA, 1); Serial.println(" uA");
}