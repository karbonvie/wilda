// ---------------------------------------------------------
// INA219 Fast Peak-Catcher (Arduino)
// ---------------------------------------------------------
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

  ina219.setCalibration_16V_400mA();

  // Override Configuration Register (0x00)
  // 0x041F sets: 16V Bus, 40mV Gain, 12-bit 1x Sample (532us), Continuous Mode
  Wire.beginTransmission(0x40);
  Wire.write(0x00);
  Wire.write(0x04);
  Wire.write(0x1F);
  Wire.endTransmission();

  Serial.println("INA219 High-Speed Peak Mode (532us conversions)");
}

void loop(void) {
  float peak_uA = 0;
  float sum_uA = 0;
  int samples = 0;
  
  uint32_t t_start = millis();
  
  // Sample as fast as possible for 1000ms
  while (millis() - t_start < 1000) {
    float current_uA = ina219.getCurrent_mA() * current_scale * 1000.0;
    
    if (current_uA > peak_uA) {
      peak_uA = current_uA;
    }
    
    sum_uA += current_uA;
    samples++;
    
    delay(1); // Keep loop stable, hardware updates every 0.5ms anyway
  }

  float avg_uA = sum_uA / samples;

  Serial.print("Avg: "); 
  Serial.print(avg_uA, 1); 
  Serial.print(" uA | PEAK: "); 
  Serial.print(peak_uA, 1); 
  Serial.println(" uA");
}
