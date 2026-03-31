#include <Arduino.h>
#include <max6675.h>

// ================= PINS =================
#define CLK_PIN 18
#define CS_PIN  5
#define DO_PIN  19
#define SSR_PIN 4

// ================= MAX6675 =================
MAX6675 thermocouple(CLK_PIN, CS_PIN, DO_PIN);

// ================= SERIAL =================
#define SERIAL_BAUD 9600

// ================= PROFILE =================
const int MAX_POINTS = 32;

struct ProfilePoint {
  float timeSec;
  float tempC;
};

ProfilePoint profile[MAX_POINTS];
int profileCount = 0;
char profileName[64] = "Untitled";

// ================= CONTROL SETTINGS =================
float pidBandC = 5.0f;         // start proportional pulsing this many degrees below target
float minDutyPercent = 20.0f;  // duty at target temperature
float controlWindowSec = 2.0f; // SSR time window
bool profileLoaded = false;

// ================= RUNTIME =================
bool runActive = false;
unsigned long runStartMs = 0;
unsigned long lastTempReadMs = 0;
unsigned long lastTelemetryMs = 0;
unsigned long windowStartMs = 0;

float currentTempC = 0.0f;
float targetTempC = 0.0f;
float currentDutyPercent = 0.0f;
bool ssrState = false;

// ================= LINE BUFFER =================
String rxLine = "";

// =====================================================
// ================= UTILITY FUNCTIONS =================
// =====================================================

float readThermocoupleC() {
  float t = thermocouple.readCelsius();
  if (isnan(t)) return currentTempC;
  return t;
}

void clearProfile() {
  profileCount = 0;
  profileLoaded = false;
  strcpy(profileName, "Untitled");
}

void sortProfileByTime() {
  for (int i = 0; i < profileCount - 1; i++) {
    for (int j = i + 1; j < profileCount; j++) {
      if (profile[j].timeSec < profile[i].timeSec) {
        ProfilePoint temp = profile[i];
        profile[i] = profile[j];
        profile[j] = temp;
      }
    }
  }
}

float getProfileDurationSec() {
  if (profileCount <= 0) return 0.0f;
  return profile[profileCount - 1].timeSec;
}

float getTargetTempAtTime(float elapsedSec) {
  if (profileCount <= 0) return 0.0f;
  if (profileCount == 1) return profile[0].tempC;

  if (elapsedSec <= profile[0].timeSec) return profile[0].tempC;
  if (elapsedSec >= profile[profileCount - 1].timeSec) return profile[profileCount - 1].tempC;

  for (int i = 0; i < profileCount - 1; i++) {
    float t0 = profile[i].timeSec;
    float t1 = profile[i + 1].timeSec;
    float y0 = profile[i].tempC;
    float y1 = profile[i + 1].tempC;

    if (elapsedSec >= t0 && elapsedSec <= t1) {
      float span = t1 - t0;
      if (span <= 0.0f) return y1;
      float frac = (elapsedSec - t0) / span;
      return y0 + frac * (y1 - y0);
    }
  }

  return profile[profileCount - 1].tempC;
}

float computeDutyPercent(float currentTemp, float targetTemp) {
  float error = targetTemp - currentTemp;

  // Overshoot protection
  if (error <= -0.3f) return 0.0f;

  // Far below target = full on
  if (error >= pidBandC) return 100.0f;

  // At or very near target = minimum duty
  if (error <= 0.0f) return minDutyPercent;

  // Proportional region
  float frac = error / pidBandC;  // 0..1
  float duty = minDutyPercent + frac * (100.0f - minDutyPercent);

  if (duty < 0.0f) duty = 0.0f;
  if (duty > 100.0f) duty = 100.0f;
  return duty;
}

void applySSRWindow(float dutyPercent) {
  unsigned long now = millis();
  unsigned long windowMs = (unsigned long)(controlWindowSec * 1000.0f);

  if (windowMs < 200) windowMs = 200;

  if (now - windowStartMs >= windowMs) {
    windowStartMs = now;
  }

  unsigned long onTimeMs = (unsigned long)(windowMs * (dutyPercent / 100.0f));
  unsigned long elapsedInWindow = now - windowStartMs;

  bool shouldBeOn = elapsedInWindow < onTimeMs;
  ssrState = shouldBeOn;
  digitalWrite(SSR_PIN, ssrState ? HIGH : LOW);
}

void stopRun() {
  runActive = false;
  currentDutyPercent = 0.0f;
  ssrState = false;
  digitalWrite(SSR_PIN, LOW);
}

void startRun() {
  if (!profileLoaded || profileCount < 1) return;
  runActive = true;
  runStartMs = millis();
  windowStartMs = millis();
}

void sendOk(const String &msg) {
  Serial.println("OK," + msg);
}

void sendErr(const String &msg) {
  Serial.println("ERR," + msg);
}

void sendTelemetry() {
  float elapsedSec = runActive ? ((millis() - runStartMs) / 1000.0f) : 0.0f;
  float durationSec = getProfileDurationSec();

  Serial.print("TELEMETRY,");
  Serial.print(millis());
  Serial.print(",");
  Serial.print(runActive ? 1 : 0);
  Serial.print(",");
  Serial.print(profileName);
  Serial.print(",");
  Serial.print(elapsedSec, 2);
  Serial.print(",");
  Serial.print(durationSec, 2);
  Serial.print(",");
  Serial.print(currentTempC, 2);
  Serial.print(",");
  Serial.print(targetTempC, 2);
  Serial.print(",");
  Serial.print(currentDutyPercent, 1);
  Serial.print(",");
  Serial.println(ssrState ? 1 : 0);
}

void sendProfile() {
  Serial.print("PROFILE,");
  Serial.print(profileName);
  Serial.print(",");
  Serial.print(profileCount);

  for (int i = 0; i < profileCount; i++) {
    Serial.print(",");
    Serial.print(profile[i].timeSec, 2);
    Serial.print(",");
    Serial.print(profile[i].tempC, 2);
  }

  Serial.println();
}

void sendSettings() {
  Serial.print("SETTINGS,");
  Serial.print(pidBandC, 2);
  Serial.print(",");
  Serial.print(minDutyPercent, 2);
  Serial.print(",");
  Serial.println(controlWindowSec, 2);
}

void sendStatus() {
  float elapsedSec = runActive ? ((millis() - runStartMs) / 1000.0f) : 0.0f;

  Serial.print("STATUS,");
  Serial.print(runActive ? 1 : 0);
  Serial.print(",");
  Serial.print(profileLoaded ? 1 : 0);
  Serial.print(",");
  Serial.print(profileName);
  Serial.print(",");
  Serial.print(profileCount);
  Serial.print(",");
  Serial.print(elapsedSec, 2);
  Serial.print(",");
  Serial.print(getProfileDurationSec(), 2);
  Serial.print(",");
  Serial.print(currentTempC, 2);
  Serial.print(",");
  Serial.print(targetTempC, 2);
  Serial.print(",");
  Serial.println(currentDutyPercent, 1);
}

bool parseProfileLine(String line) {
  // FORMAT:
  // PROFILE,<name>,<count>,<time0>,<temp0>,<time1>,<temp1>...
  clearProfile();

  line.trim();
  int firstComma = line.indexOf(',');
  if (firstComma < 0) {
    sendErr("Bad PROFILE format");
    return false;
  }

  String payload = line.substring(firstComma + 1);

  int comma1 = payload.indexOf(',');
  int comma2 = payload.indexOf(',', comma1 + 1);
  if (comma1 < 0 || comma2 < 0) {
    sendErr("Bad PROFILE header");
    return false;
  }

  String nameStr = payload.substring(0, comma1);
  String countStr = payload.substring(comma1 + 1, comma2);

  nameStr.trim();
  countStr.trim();

  int count = countStr.toInt();
  if (count < 1 || count > MAX_POINTS) {
    sendErr("Invalid profile point count");
    return false;
  }

  nameStr.toCharArray(profileName, sizeof(profileName));

  String rest = payload.substring(comma2 + 1);

  int idx = 0;
  while (rest.length() > 0 && idx < count) {
    int c1 = rest.indexOf(',');
    if (c1 < 0) {
      sendErr("Missing temperature value");
      clearProfile();
      return false;
    }

    String timeStr = rest.substring(0, c1);
    rest = rest.substring(c1 + 1);

    String tempStr;
    int c2 = rest.indexOf(',');
    if (c2 < 0) {
      tempStr = rest;
      rest = "";
    } else {
      tempStr = rest.substring(0, c2);
      rest = rest.substring(c2 + 1);
    }

    timeStr.trim();
    tempStr.trim();

    profile[idx].timeSec = timeStr.toFloat();
    profile[idx].tempC = tempStr.toFloat();
    idx++;
  }

  if (idx != count) {
    clearProfile();
    sendErr("Point count mismatch");
    return false;
  }

  profileCount = count;
  sortProfileByTime();
  profileLoaded = true;

  sendOk("Profile loaded");
  sendProfile();
  return true;
}

void handleCommand(String line) {
  line.trim();
  if (line.length() == 0) return;

  if (line == "PING") {
    sendOk("PONG");
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

  if (line == "GET_SETTINGS") {
    sendSettings();
    return;
  }

  if (line == "START") {
    if (!profileLoaded || profileCount < 1) {
      sendErr("No profile loaded");
      return;
    }
    startRun();
    sendOk("Run started");
    sendStatus();
    return;
  }

  if (line == "STOP") {
    stopRun();
    sendOk("Run stopped");
    sendStatus();
    return;
  }

  if (line.startsWith("SET_PID_BAND,")) {
    String v = line.substring(String("SET_PID_BAND,").length());
    pidBandC = v.toFloat();
    if (pidBandC < 0.1f) pidBandC = 0.1f;
    sendOk("PID band updated");
    sendSettings();
    return;
  }

  if (line.startsWith("SET_MIN_DUTY,")) {
    String v = line.substring(String("SET_MIN_DUTY,").length());
    minDutyPercent = v.toFloat();
    if (minDutyPercent < 0.0f) minDutyPercent = 0.0f;
    if (minDutyPercent > 100.0f) minDutyPercent = 100.0f;
    sendOk("Min duty updated");
    sendSettings();
    return;
  }

  if (line.startsWith("SET_WINDOW,")) {
    String v = line.substring(String("SET_WINDOW,").length());
    controlWindowSec = v.toFloat();
    if (controlWindowSec < 0.2f) controlWindowSec = 0.2f;
    if (controlWindowSec > 30.0f) controlWindowSec = 30.0f;
    sendOk("Window updated");
    sendSettings();
    return;
  }

  if (line.startsWith("PROFILE,")) {
    parseProfileLine(line);
    return;
  }

  sendErr("Unknown command");
}

void readSerialLines() {
  while (Serial.available()) {
    char c = (char)Serial.read();

    if (c == '\r') continue;

    if (c == '\n') {
      handleCommand(rxLine);
      rxLine = "";
    } else {
      rxLine += c;
      if (rxLine.length() > 800) rxLine = "";
    }
  }
}

// =====================================================
// ======================= SETUP ========================
// =====================================================

void setup() {
  pinMode(SSR_PIN, OUTPUT);
  digitalWrite(SSR_PIN, LOW);

  Serial.begin(SERIAL_BAUD);
  delay(500);

  currentTempC = readThermocoupleC();
  targetTempC = currentTempC;

  Serial.println("HEADINGS,time_ms,run_active,profile_name,elapsed_s,total_s,current_temp_c,target_temp_c,duty_percent,ssr_on");
  sendOk("ESP32 carbon cure controller ready");
  sendStatus();
  sendSettings();
}

// =====================================================
// ======================== LOOP ========================
// =====================================================

void loop() {
  readSerialLines();

  unsigned long now = millis();

  if (now - lastTempReadMs >= 500) {
    lastTempReadMs = now;
    currentTempC = readThermocoupleC();
  }

  if (runActive) {
    float elapsedSec = (now - runStartMs) / 1000.0f;
    float durationSec = getProfileDurationSec();

    if (elapsedSec >= durationSec) {
      targetTempC = getTargetTempAtTime(durationSec);
      currentDutyPercent = computeDutyPercent(currentTempC, targetTempC);
      applySSRWindow(currentDutyPercent);

      // end run after final point
      stopRun();
      sendOk("Profile finished");
      sendStatus();
    } else {
      targetTempC = getTargetTempAtTime(elapsedSec);
      currentDutyPercent = computeDutyPercent(currentTempC, targetTempC);
      applySSRWindow(currentDutyPercent);
    }
  } else {
    currentDutyPercent = 0.0f;
    digitalWrite(SSR_PIN, LOW);
    ssrState = false;
  }

  if (now - lastTelemetryMs >= 500) {
    lastTelemetryMs = now;
    sendTelemetry();
  }
}
