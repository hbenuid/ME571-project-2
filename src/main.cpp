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

// Waveguide reading at rest, used as the reference for optical loss
double waveguideBaseline = 0;

// Absolute pressure with the actuator vented, subtracted to give bladder (gauge) pressure
float pressureBaseline = 0;
float bladderPsi = 0;  // Latest bladder (gauge) pressure

// Angle calibration: angle (deg) = ANGLE_A * loss^2 + ANGLE_B * loss + ANGLE_C, loss in dB.
// Placeholder fit. Replace with coefficients measured on this sleeve.
const double ANGLE_A = 3.1412;
const double ANGLE_B = 84.948;
const double ANGLE_C = 1.1066;

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

// Act on one character from the dashboard. Anything else, such as line endings, is ignored.
void handleCommand(char command) {
  switch (command) {
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

  // Take the baselines once the actuator has vented.
  waveguideBaseline = analogRead(WAVEGUIDE_PIN);
  pressureBaseline = readPressurePsi();

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
  bool lossValid = waveguideADC > 0 && waveguideBaseline > 0;
  double loss = lossValid ? 10 * log10(waveguideBaseline / waveguideADC) : 0;  // Optical loss in dB

  double angle = ANGLE_A * sq(loss) + ANGLE_B * loss + ANGLE_C;  // Bending angle in degrees
  if (angle < 0) angle = 0;

  // The dashboard (index.html) finds columns by these header names.
  if (rowCount % HEADER_EVERY == 0) {
    Serial.println();
    Serial.println(" Force(g)  Pump  Mode  Press(psi)  Loss(dB)  Angle(deg)  Wave(V)");
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
  Serial.println();
}
