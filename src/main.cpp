#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <Adafruit_MPRLS.h>

const int RESET_PIN = -1;
const int EOC_PIN = -1;
Adafruit_MPRLS mpr(RESET_PIN, EOC_PIN);

const int pumpPin = 3;
const int solenoid = 4;
const int FSR_PIN = A0;
const int WAVEGUIDE_PIN = A1;  // Soft optical waveguide input

// Assumes D4 HIGH closes the exhaust. Verify this on your valve.
const byte VALVE_CLOSED = HIGH;
const byte VALVE_OPEN = LOW;

// FSR thresholds in estimated grams
const float PRESS_FORCE = 600.0f;
const float RELEASE_FORCE = 300.0f;
const float R_DIV = 3000.0f;  // Fixed resistor from A0 to GND, in ohms

const unsigned long VALVE_DELAY_MS = 50;
bool inflating = false;

// Waveguide reading at rest, used as the reference for optical loss
double waveguideBaseline = 0;

float readForce() {
  int fsrADC = analogRead(FSR_PIN);

  if (fsrADC == 0) {
    return 0.0f;
  }

  // Avoid division by zero when the ADC reaches full scale.
  if (fsrADC == 1023) {
    fsrADC = 1022;
  }

  float fsrResistance = R_DIV * (1023.0f / fsrADC - 1.0f);
  float conductance = 1.0f / fsrResistance;

  // Force estimate from the original lab sketch
  if (fsrResistance <= 600.0f) {
    return (conductance - 0.00075f) / 0.00000032639f;
  }

  return conductance / 0.000000642857f;
}

void setup() {
  pinMode(pumpPin, OUTPUT);
  pinMode(solenoid, OUTPUT);

  digitalWrite(pumpPin, LOW);
  digitalWrite(solenoid, VALVE_OPEN);

  Serial.begin(115200);

  if (!mpr.begin()) {
    Serial.println("MPRLS sensor not found. Check wiring and reset.");
    while (true) {
      delay(100);
    }
  }

  Serial.println("Venting for 5 seconds. Leave the FSR released.");
  delay(5000);

  // Take the optical baseline once the actuator has vented.
  waveguideBaseline = analogRead(WAVEGUIDE_PIN);

  Serial.println("Ready. Press and hold the FSR to inflate.");
}

void loop() {
  float forceVal = readForce();

  if (!inflating && forceVal > PRESS_FORCE) {
    // Close the exhaust before turning on the pump.
    digitalWrite(solenoid, VALVE_CLOSED);
    delay(VALVE_DELAY_MS);

    // Check that the FSR is still being held.
    forceVal = readForce();
    if (forceVal > RELEASE_FORCE) {
      digitalWrite(pumpPin, HIGH);
      inflating = true;
    } else {
      digitalWrite(solenoid, VALVE_OPEN);
    }
  } else if (inflating && forceVal <= RELEASE_FORCE) {
    // Stop the pump before opening the exhaust.
    digitalWrite(pumpPin, LOW);
    digitalWrite(solenoid, VALVE_OPEN);
    inflating = false;
  }

  float pressure_hPa = mpr.readPressure();

  int waveguideADC = analogRead(WAVEGUIDE_PIN);
  float waveguideVoltage = waveguideADC * (5.0f / 1023.0f);
  double loss = 10 * log10(waveguideBaseline / waveguideADC);  // Optical loss in dB

  // double angle = 3.1412 * sq(loss) + 84.948 * loss + 1.1066;  // Calibration: angle vs loss
  // if (angle < 0) angle = 0;

  Serial.print("Force (estimated g): ");
  Serial.print(forceVal, 0);
  Serial.print(" | Pump command: ");
  Serial.print(inflating ? "ON" : "OFF");
  Serial.print(" | Pressure (absolute PSI): ");
  Serial.print(pressure_hPa / 68.9476f, 2);
  Serial.print(" | Loss (dB): ");
  Serial.print(loss, 2);
  Serial.print(" | Waveguide voltage (V): ");
  Serial.println(waveguideVoltage, 2);
}
