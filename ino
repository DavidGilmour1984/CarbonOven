#include <max6675.h>

// ================= SERIAL =================
#define SERIAL_BAUD 9600

// ================= PINS =================
#define SSR_PIN          4
#define THERMO_SCK_PIN  18
#define THERMO_CS_PIN    5
#define THERMO_SO_PIN   19

// ================= THERMOCOUPLE =================
MAX6675 thermocouple(THERMO_SCK_PIN, THERMO_CS_PIN, THERMO_SO_PIN);

// ================= RUN PROFILE =================
const int MAX_POINTS = 20;

struct ProfilePoint {
  uint32_t timeSec;   // cumulative time from start
  float tempC;
};

ProfilePoint profile[MAX_POINTS];
int profileCount = 0;

// ================= CONTROL SETTINGS =================
float proportionalBand = 20.0f;   // degrees below target where proportional control begins
float proportionalCurve = 1.5f;   // >1 makes shorter pulses nearer target
uint32_t windowMs = 2000;         // SSR time-proportioning window

// ================= RUN STATE =================
bool runActive = false;
bool configLoaded = false;
uint32_t runStartMs = 0;
uint32_t windowStartMs = 0;

float currentTemp = 0.0f;
float currentTarget = 0.0f;
float currentDuty = 0.0f;
bool heaterOn = false;
int currentSegment = 0;

// ================= TIMING =================
uint32_t lastTempReadMs = 0;
uint32_t lastStatusMs = 0;
const uint32_t tempReadIntervalMs = 250;
const uint32_t statusIntervalMs   = 500;

// ================= SERIAL BUFFER =================
String rxLine = "";

// ===================================================
// ================= HELPER FUNCTIONS =================
// ===================================================

void setHeater(bool on) {
  heaterOn = on;
  digitalWrite(SSR_PIN, on ? HIGH : LOW);
}

void clearProfile() {
  profileCount = 0;
  configLoaded = false;
  runActive = false;
  currentSegment = 0;
  currentTarget = 0.0f;
  currentDuty = 0.0f;
  setHeater(false);
}

bool addProfilePoint(uint32_t timeSec, float tempC) {
  if (profileCount >= MAX_POINTS) return false;

  if (profileCount > 0) {
    if (timeSec <= profile[profileCount - 1].timeSec) {
      return false;
    }
  }

  profile[profileCount].timeSec = timeSec;
  profile[profileCount].tempC = tempC;
  profileCount++;
  configLoaded = (profileCount >= 2);
  return true;
}

float readTemperatureC() {
  float t = thermocouple.readCelsius();

  if (isnan(t) || t < -50 || t > 1400) {
    return currentTemp;
  }

  return t;
}

float getTargetAtTime(uint32_t elapsedSec, int &segmentOut) {
  if (profileCount == 0) {
    segmentOut = 0;
    return 0.0f;
  }

  if (elapsedSec <= profile[0].timeSec) {
    segmentOut = 0;
    return profile[0].tempC;
  }

  for (int i = 0; i < profileCount - 1; i++) {
    uint32_t t0 = profile[i].timeSec;
    uint32_t t1 = profile[i + 1].timeSec;
    float y0 = profile[i].tempC;
    float y1 = profile[i + 1].tempC;

    if (elapsedSec >= t0 && elapsedSec <= t1) {
      float frac = float(elapsedSec - t0) / float(t1 - t0);
      segmentOut = i + 1;
      return y0 + frac * (y1 - y0);
    }
  }

  segmentOut = profileCount - 1;
  return profile[profileCount - 1].tempC;
}

float computeDuty(float targetC, float actualC) {
  float error = targetC - actualC;

  if (error <= 0.0f) return 0.0f;
  if (proportionalBand <= 0.01f) return (error > 0.0f) ? 1.0f : 0.0f;

  if (error >= proportionalBand) return 1.0f;

  float x = error / proportionalBand;
  if (x < 0.0f) x = 0.0f;
  if (x > 1.0f) x = 1.0f;

  float duty = powf(x, proportionalCurve);

  if (duty < 0.0f) duty = 0.0f;
  if (duty > 1.0f) duty = 1.0f;

  return duty;
}

void stopRun(bool sendDone) {
  runActive = false;
  currentDuty = 0.0f;
  setHeater(false);

  if (sendDone) {
    Serial.println("DONE");
  }
}

void startRun() {
  if (!configLoaded || profileCount < 2) {
    Serial.println("ERR,NO_VALID_PROFILE");
    return;
  }

  runActive = true;
  runStartMs = millis();
  windowStartMs = millis();
  currentSegment = 0;
  Serial.println("ACK,STARTED");
}

void updateRunControl() {
  if (!runActive) {
    setHeater(false);
    return;
  }

  uint32_t now = millis();
  uint32_t elapsedMs = now - runStartMs;
  uint32_t elapsedSec = elapsedMs / 1000UL;

  uint32_t endSec = profile[profileCount - 1].timeSec;
  if (elapsedSec >= endSec) {
    currentTarget = profile[profileCount - 1].tempC;
    stopRun(true);
    return;
  }

  currentTarget = getTargetAtTime(elapsedSec, currentSegment);
  currentDuty = computeDuty(currentTarget, currentTemp);

  if ((now - windowStartMs) >= windowMs) {
    windowStartMs = now;
  }

  uint32_t inWindow = now - windowStartMs;
  uint32_t onTime = (uint32_t)(currentDuty * windowMs);

  if (inWindow < onTime) {
    setHeater(true);
  } else {
    setHeater(false);
  }
}

void sendStatus() {
  uint32_t now = millis();
  uint32_t runSec = runActive ? ((now - runStartMs) / 1000UL) : 0UL;

  Serial.print("STATUS,");
  Serial.print(runActive ? 1 : 0);
  Serial.print(",");
  Serial.print(runSec);
  Serial.print(",");
  Serial.print(currentSegment);
  Serial.print(",");
  Serial.print(currentTemp, 2);
  Serial.print(",");
  Serial.print(currentTarget, 2);
  Serial.print(",");
  Serial.print(currentDuty, 3);
  Serial.print(",");
  Serial.print(heaterOn ? 1 : 0);
  Serial.print(",");
  Serial.print(proportionalBand, 2);
  Serial.print(",");
  Serial.print(proportionalCurve, 2);
  Serial.print(",");
  Serial.println(windowMs);
}

void sendProfile() {
  Serial.print("PROFILE_COUNT,");
  Serial.println(profileCount);

  for (int i = 0; i < profileCount; i++) {
    Serial.print("PROFILE,");
    Serial.print(i);
    Serial.print(",");
    Serial.print(profile[i].timeSec);
    Serial.print(",");
    Serial.println(profile[i].tempC, 2);
  }
}

void processCommand(String line) {
  line.trim();
  if (line.length() == 0) return;

  if (line == "PING") {
    Serial.println("ACK,PONG");
    return;
  }

  if (line == "CLEAR") {
    clearProfile();
    Serial.println("ACK,CLEARED");
    return;
  }

  if (line == "START") {
    startRun();
    return;
  }

  if (line == "STOP") {
    stopRun(false);
    Serial.println("ACK,STOPPED");
    return;
  }

  if (line == "GET_STATUS") {
    sendStatus();
    return;
  }

  if (line == "GET_PROFILE") {
    sendProfile();
    return;
  }

  if (line.startsWith("STEP,")) {
    int c1 = line.indexOf(',');
    int c2 = line.indexOf(',', c1 + 1);

    if (c1 < 0 || c2 < 0) {
      Serial.println("ERR,BAD_STEP_FORMAT");
      return;
    }

    uint32_t timeSec = (uint32_t)line.substring(c1 + 1, c2).toInt();
    float tempC = line.substring(c2 + 1).toFloat();

    if (!addProfilePoint(timeSec, tempC)) {
      Serial.println("ERR,STEP_REJECTED");
      return;
    }

    Serial.print("ACK,STEP_ADDED,");
    Serial.print(timeSec);
    Serial.print(",");
    Serial.println(tempC, 2);
    return;
  }

  if (line.startsWith("SETTINGS,")) {
    int c1 = line.indexOf(',');
    int c2 = line.indexOf(',', c1 + 1);
    int c3 = line.indexOf(',', c2 + 1);

    if (c1 < 0 || c2 < 0 || c3 < 0) {
      Serial.println("ERR,BAD_SETTINGS_FORMAT");
      return;
    }

    float newBand = line.substring(c1 + 1, c2).toFloat();
    float newCurve = line.substring(c2 + 1, c3).toFloat();
    uint32_t newWindow = (uint32_t)line.substring(c3 + 1).toInt();

    if (newBand < 0.1f) newBand = 0.1f;
    if (newCurve < 0.2f) newCurve = 0.2f;
    if (newWindow < 250) newWindow = 250;
    if (newWindow > 10000) newWindow = 10000;

    proportionalBand = newBand;
    proportionalCurve = newCurve;
    windowMs = newWindow;

    Serial.print("ACK,SETTINGS,");
    Serial.print(proportionalBand, 2);
    Serial.print(",");
    Serial.print(proportionalCurve, 2);
    Serial.print(",");
    Serial.println(windowMs);
    return;
  }

  Serial.println("ERR,UNKNOWN_COMMAND");
}

void readSerialLines() {
  while (Serial.available()) {
    char ch = (char)Serial.read();

    if (ch == '\n') {
      processCommand(rxLine);
      rxLine = "";
    } else if (ch != '\r') {
      rxLine += ch;
      if (rxLine.length() > 200) {
        rxLine = "";
        Serial.println("ERR,LINE_TOO_LONG");
      }
    }
  }
}

// ===================================================
// ====================== SETUP =======================
// ===================================================

void setup() {
  pinMode(SSR_PIN, OUTPUT);
  setHeater(false);

  Serial.begin(SERIAL_BAUD);
  delay(500);

  currentTemp = readTemperatureC();

  Serial.println("READY");
  Serial.println("COMMANDS:CLEAR|STEP,timeSec,tempC|SETTINGS,band,curve,windowMs|START|STOP|GET_STATUS|GET_PROFILE");
}

// ===================================================
// ======================= LOOP =======================
// ===================================================

void loop() {
  readSerialLines();

  uint32_t now = millis();

  if ((now - lastTempReadMs) >= tempReadIntervalMs) {
    lastTempReadMs = now;
    currentTemp = readTemperatureC();
  }

  updateRunControl();

  if ((now - lastStatusMs) >= statusIntervalMs) {
    lastStatusMs = now;
    sendStatus();
  }
}
