#include <Arduino.h>
#include <EEPROM.h>
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

// D4 HIGH closes the exhaust (checked on our valve).
const byte VALVE_CLOSED = HIGH;
const byte VALVE_OPEN = LOW;

// FSR thresholds in estimated grams
const float PRESS_FORCE = 600.0f;
const float RELEASE_FORCE = 300.0f;
const float R_DIV = 3000.0f;  // Fixed resistor from A0 to GND, in ohms

const unsigned long VALVE_DELAY_MS = 50;
bool inflating = false;  // FSR mode: pump running because the FSR is held
bool fsrArmed = true;    // FSR mode: false until the FSR is released after dashboard control

// Dashboard control. index.html sends single characters over serial:
//   I = inflate, H = hold, V = vent, K = keep-alive, X = hand control back to the FSR.
// While the dashboard is in control, pressing the FSR vents the bladder (stop button).
const float MAX_DASHBOARD_PSI = 30.0f;            // Above the ~10 psi gauge the MPRLS can read, so a valid
                                                  // reading never reaches it. Only a failed (NaN) reading stops it.
const unsigned long DASHBOARD_TIMEOUT_MS = 1500;  // Vent and hand back to the FSR if the dashboard goes quiet

enum Mode { MODE_FSR, MODE_INFLATE, MODE_HOLD, MODE_VENT, MODE_STOP, MODE_MAX };
const char *const MODE_LABELS[] = { "FSR", "INFL", "HOLD", "VENT", "STOP", "MAX" };  // Mode column
Mode mode = MODE_FSR;
unsigned long lastCommandMs = 0;

// Reference for the optical loss: the A1 reading (ADC counts) with the finger straight. It isn't
// measured at startup, so the loss and angle mean the same in every session. The light level must
// stay the same for that to hold: changing the LED, potentiometer or wiring shifts every angle.
// The dashboard's Debug view sets it with "B<counts>\n", and it's saved in EEPROM, which keeps it
// through restarts and uploads. The default is used only until one is saved: 787 counts (3.85 V) is
// the straight-finger reading from the 2026-11-10 calibration.
const int DEFAULT_WAVEGUIDE_BASELINE = 787;
const int BASELINE_EEPROM_ADDR = 0;
const uint16_t BASELINE_EEPROM_MAGIC = 0xB5E1;  // marks a saved baseline
struct SavedBaseline {
  uint16_t magic;
  uint16_t counts;
};
int waveguideBaseline = DEFAULT_WAVEGUIDE_BASELINE;
long baselineEntry = -1;  // digits received so far after "B", or -1 when not reading one

// Absolute pressure with the actuator vented, subtracted to give bladder (gauge) pressure
float pressureBaseline = 0;
float bladderPsi = 0;  // Latest bladder (gauge) pressure

// Angle calibration: angle (deg) = ANGLE_A * loss^2 + ANGLE_B * loss + ANGLE_C, loss in dB.
// Fitted 2026-11-10 from 5 points, 1.64 to 5.32 dB and 0 to 90 deg. RMS error 7.4 deg.
// Measured against a waveguide baseline of 787 counts: it holds only while that baseline is saved.
const double ANGLE_A = 2.241;
const double ANGLE_B = 9.35217;
const double ANGLE_C = -21.9498;

// Serial output
const unsigned long PRINT_INTERVAL_MS = 200;  // 5 rows per second
const unsigned long HEADER_EVERY = 20;        // Rows between repeated headers
unsigned long lastPrintMs = 0;
unsigned long rowCount = 0;

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

float readPressurePsi() {
  return mpr.readPressure() / 68.9476f;  // hPa to absolute PSI
}

// Switch modes and drive the pump and valve to match. The exhaust closes
// before the pump starts, and the pump stops before the exhaust opens.
void setMode(Mode next) {
  if (next == mode) {
    return;
  }
  mode = next;
  inflating = false;

  if (next == MODE_INFLATE) {
    digitalWrite(solenoid, VALVE_CLOSED);
    delay(VALVE_DELAY_MS);
    digitalWrite(pumpPin, HIGH);
    return;
  }

  digitalWrite(pumpPin, LOW);
  bool holding = next == MODE_HOLD || next == MODE_MAX;
  digitalWrite(solenoid, holding ? VALVE_CLOSED : VALVE_OPEN);

  if (next == MODE_FSR) {
    fsrArmed = false;  // Don't inflate until the FSR has been released.
  }
}

// The dashboard's Debug view reads this line.
void printWaveguideBaseline(const char *source) {
  Serial.print("Waveguide baseline: ");
  Serial.print(waveguideBaseline);
  Serial.print(" counts (");
  Serial.print(waveguideBaseline * 5.0 / 1023.0, 2);
  Serial.print(" V), ");
  Serial.println(source);
}

void loadWaveguideBaseline() {
  SavedBaseline saved;
  EEPROM.get(BASELINE_EEPROM_ADDR, saved);
  if (saved.magic == BASELINE_EEPROM_MAGIC && saved.counts >= 1 && saved.counts <= 1023) {
    waveguideBaseline = saved.counts;
    printWaveguideBaseline("saved on the Arduino");
  } else {
    waveguideBaseline = DEFAULT_WAVEGUIDE_BASELINE;
    printWaveguideBaseline("default in src/main.cpp");
  }
}

void saveWaveguideBaseline(int counts) {
  waveguideBaseline = counts;
  SavedBaseline saved = { BASELINE_EEPROM_MAGIC, (uint16_t)counts };
  EEPROM.put(BASELINE_EEPROM_ADDR, saved);  // writes only the bytes that changed
  printWaveguideBaseline("saved");
}

// Act on one character from the dashboard. Anything else, such as line endings, is ignored.
// "B" starts a baseline: its digits and a newline follow, as in "B787\n".
void handleCommand(char command) {
  if (baselineEntry >= 0) {
    if (command >= '0' && command <= '9') {
      baselineEntry = min(baselineEntry * 10 + (command - '0'), 100000L);
      return;
    }
    if (command == '\n' || command == '\r') {
      if (baselineEntry >= 1 && baselineEntry <= 1023) {
        saveWaveguideBaseline((int)baselineEntry);
      } else {
        Serial.println("Baseline not changed: it must be 1 to 1023 counts.");
      }
      baselineEntry = -1;
      return;
    }
    baselineEntry = -1;  // anything else cancels the baseline and is handled as usual
  }
  switch (command) {
    case 'B':
      baselineEntry = 0;
      return;
    case 'I':
      setMode(bladderPsi < MAX_DASHBOARD_PSI ? MODE_INFLATE : MODE_MAX);
      break;
    case 'H':
      setMode(MODE_HOLD);
      break;
    case 'V':
      setMode(MODE_VENT);
      break;
    case 'K':
      break;
    case 'X':
      setMode(MODE_FSR);
      break;
    default:
      return;
  }
  lastCommandMs = millis();
}

// Original behaviour: hold the FSR to inflate, release it to vent.
void runFsrControl(float forceVal) {
  if (forceVal <= RELEASE_FORCE) {
    fsrArmed = true;
  }

  if (!inflating && fsrArmed && forceVal > PRESS_FORCE) {
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
}

// Print a number right-aligned in a column of the given width.
void printValue(float value, int width, int decimals) {
  char buf[16];
  dtostrf(value, width, decimals, buf);
  Serial.print(buf);
}

// Print text right-aligned in a column of the given width.
void printText(const char *text, int width) {
  for (int i = strlen(text); i < width; i++) {
    Serial.print(' ');
  }
  Serial.print(text);
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

  // Take the pressure baseline once the actuator has vented. The waveguide baseline is saved.
  pressureBaseline = readPressurePsi();

  loadWaveguideBaseline();

  Serial.println("Ready. Press and hold the FSR to inflate.");
}

void loop() {
  float forceVal = readForce();

  while (Serial.available() > 0) {
    handleCommand(Serial.read());
  }

  if (mode == MODE_FSR) {
    runFsrControl(forceVal);
  } else if (millis() - lastCommandMs > DASHBOARD_TIMEOUT_MS) {
    setMode(MODE_FSR);  // Dashboard went quiet: vent and hand back to the FSR.
  } else if (forceVal > PRESS_FORCE) {
    setMode(MODE_STOP);  // The FSR is a stop button while the dashboard is in control.
  } else if (mode == MODE_INFLATE) {
    // Check the limit on every pass while the pump runs, not only when printing.
    bladderPsi = readPressurePsi() - pressureBaseline;
    if (!(bladderPsi < MAX_DASHBOARD_PSI)) {  // Also stops on a failed (NaN) reading
      setMode(MODE_MAX);
    }
  }

  // Print at a fixed rate; the pump control above still runs every loop.
  unsigned long now = millis();
  if (now - lastPrintMs < PRINT_INTERVAL_MS) {
    return;
  }
  lastPrintMs = now;

  bladderPsi = readPressurePsi() - pressureBaseline;

  int waveguideADC = analogRead(WAVEGUIDE_PIN);
  float waveguideVoltage = waveguideADC * (5.0f / 1023.0f);

  // Loss is undefined when either reading is zero.
  bool lossValid = waveguideADC > 0;
  double loss = lossValid ? 10 * log10((double)waveguideBaseline / waveguideADC) : 0;  // Optical loss in dB

  double angle = ANGLE_A * sq(loss) + ANGLE_B * loss + ANGLE_C;  // Bending angle in degrees
  if (angle < 0) angle = 0;

  // The dashboard (index.html) finds columns by these header names.
  if (rowCount % HEADER_EVERY == 0) {
    Serial.println();
    Serial.println(" Force(g)  Pump  Mode  Press(psi)  Loss(dB)  Angle(deg)  Wave(V)  Wave(raw)");
  }
  rowCount++;

  printValue(forceVal, 9, 0);
  printText(digitalRead(pumpPin) == HIGH ? "ON" : "OFF", 6);
  printText(MODE_LABELS[mode], 6);
  printValue(bladderPsi, 12, 2);
  if (lossValid) {
    printValue(loss, 10, 2);
    printValue(angle, 12, 1);
  } else {
    printText("--", 10);
    printText("--", 12);
  }
  printValue(waveguideVoltage, 9, 2);
  printValue(waveguideADC, 11, 0);  // A1 in ADC counts, 0 to 1023
  Serial.println();
}
