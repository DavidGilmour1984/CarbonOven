#include <WiFi.h>
#include <HTTPClient.h>
#include <max6675.h>

// ================= WIFI =================
#define WIFI_SSID     "G2.4"
#define WIFI_PASSWORD ""

#define SCRIPT_URL "https://script.googlAVtVT3enoVKnchQYQ66g/exec"
#define DEVICE_ID "oven_2"

// ================= SERIAL =================
#define SERIAL_BAUD 9600

// ================= PINS =================
#define INDICATOR_PIN   2   // D2
#define SSR_PIN         4   // D4

#define THERMO_SCK_PIN  18
#define THERMO_CS_PIN    5
#define THERMO_SO_PIN   19

MAX6675 thermocouple(THERMO_SCK_PIN, THERMO_CS_PIN, THERMO_SO_PIN);

// ================= PROFILE =================
struct Step {
  float temp;
  int duration;
};

Step profile[] = {
  {20, 30},  // Settle time (20))
  {75, 1800},  // Ramp to 75°C (30 min)
  {75, 3600},  // Soak at 75°C (60 min)
  {125, 1800}, // Ramp to 125°C (30 min)
  {125, 3600}  // Soak at 125°C (60 min)
};

const int NUM_STEPS = sizeof(profile) / sizeof(profile[0]);

bool running = false;
unsigned long startTime = 0;
unsigned long lastControl = 0;
unsigned long lastUpload = 0;
unsigned long lastWiFiCheck = 0;

// ================= OUTPUT CONTROL =================
void setOutputs(bool state) {
  digitalWrite(INDICATOR_PIN, state);
  digitalWrite(SSR_PIN, state);
}

// ================= TARGET =================
float getTargetTemp(unsigned long t) {
  unsigned long elapsed = t / 1000;
  int cumulative = 0;

  for (int i = 0; i < NUM_STEPS; i++) {
    float startTemp = (i == 0) ? profile[0].temp : profile[i-1].temp;
    float endTemp = profile[i].temp;

    if (elapsed < cumulative + profile[i].duration) {
      float progress = float(elapsed - cumulative) / profile[i].duration;
      return startTemp + progress * (endTemp - startTemp);
    }

    cumulative += profile[i].duration;
  }

  return profile[NUM_STEPS - 1].temp;
}

int getTotalTime() {
  int total = 0;
  for (int i = 0; i < NUM_STEPS; i++) total += profile[i].duration;
  return total;
}

// ================= SSR =================
void controlSSR(float currentTemp, float targetTemp) {

  float error = targetTemp - currentTemp;

  // ===== HARD CUTOFF (1°C OVER) =====
  if (currentTemp >= targetTemp + 1.0) {
    setOutputs(false);
    Serial.println("SSR OFF (over +1C)");
    return;
  }

  // ===== BASE DUTY =====
  float duty;

  // If below target → ramp up
  if (error > 5.0) {
    duty = 1.0;  // full power
  }
  else if (error > 0) {
    // scale from 20% → 100%
    duty = 0.2 + (error / 5.0) * 0.8;
  }
  else {
    // at or slightly below target → HOLD at 20%
    duty = 0.2;
  }

  // ===== PWM WINDOW =====
  int onTime = duty * 1000;

  if (millis() % 1000 < onTime) {
    setOutputs(true);
    Serial.println("SSR ON");
  } else {
    setOutputs(false);
    Serial.println("SSR OFF (PWM)");
  }

  Serial.println("ERROR: " + String(error));
  Serial.println("DUTY: " + String(duty));
}

// ================= GOOGLE =================
void sendData(String csv, float temp, int timeLeft) {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected - skip send");
    return;
  }

  HTTPClient http;

  String url = String(SCRIPT_URL) +
  "?type=log" +
  "&device=" + String(DEVICE_ID) +
  "&csv=" + csv +
  "&temp=" + String(temp) +
  "&timeleft=" + String(timeLeft) +
  "&t=" + String(millis());

  Serial.println("📤 SENDING:");
  Serial.println(url);

  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(5000);

  int code = http.GET();

  Serial.println("HTTP CODE: " + String(code));

  if (code > 0) {
    String payload = http.getString();
    Serial.println("RESPONSE: " + payload);
  } else {
    Serial.println("❌ HTTP FAILED");
  }

  http.end();
}

String getCommand() {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected - cannot get command");
    return "";
  }

  HTTPClient http;

  String url = String(SCRIPT_URL) +
  "?type=control" +
  "&device=" + String(DEVICE_ID) +
  "&t=" + String(millis());

  Serial.println("📥 GET CMD:");
  Serial.println(url);

  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(5000);

  int code = http.GET();

  Serial.println("CMD HTTP CODE: " + String(code));

  if (code > 0) {
    String payload = http.getString();
    payload.trim();

    Serial.println("CMD RECEIVED: [" + payload + "]");

    http.end();
    return payload;
  }

  Serial.println("❌ FAILED TO GET COMMAND");
  http.end();
  return "";
}

// ================= SETUP =================
void setup() {
  Serial.begin(SERIAL_BAUD);

  pinMode(INDICATOR_PIN, OUTPUT);
  pinMode(SSR_PIN, OUTPUT);

  setOutputs(false);

  Serial.println("BOOTING...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("CONNECTING");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\n✅ WIFI CONNECTED");
  Serial.println(WiFi.localIP());
}

// ================= LOOP =================
void loop() {

  // ===== WIFI KEEP ALIVE =====
  if (millis() - lastWiFiCheck > 10000) {
    lastWiFiCheck = millis();

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("⚠️ WiFi lost - reconnecting...");
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }

  // ===== GET COMMAND =====
  static bool runLocked = false;
  static String lastCmd = "";

  String cmd = getCommand();

  bool newCommand = (cmd != lastCmd);
  lastCmd = cmd;

  // ===== RUN =====
  if (cmd == "RUN" && !running && !runLocked && newCommand) {
    running = true;
    startTime = millis();
    Serial.println("🔥 RUN STARTED");
  }

  // ===== STOP =====
  else if (cmd == "STOP" && newCommand) {
    running = false;
    runLocked = false;
    setOutputs(false);
    Serial.println("🛑 STOPPED (UNLOCKED)");
  }

  else {
    Serial.println("CMD: " + cmd);
  }

  // ===== CONTROL LOOP =====
  if (running && millis() - lastControl >= 1000) {

    lastControl = millis();

    unsigned long t = millis() - startTime;
    int totalTime = getTotalTime();

    float currentTemp = thermocouple.readCelsius();
    float targetTemp = getTargetTemp(t);

    Serial.println("---- CONTROL ----");
    Serial.println("TIME: " + String(t / 1000));
    Serial.println("CURRENT: " + String(currentTemp));
    Serial.println("TARGET: " + String(targetTemp));

    if (t / 1000 >= totalTime) {
      running = false;
      runLocked = true;
      setOutputs(false);
      Serial.println("✅ PROFILE COMPLETE (LOCKED)");
      return;
    }

    controlSSR(currentTemp, targetTemp);
  }

  // ===== SEND DATA =====
  if (millis() - lastUpload >= 20000) {

    lastUpload = millis();

    float currentTemp = thermocouple.readCelsius();

    int timeLeft = 0;
    float targetTemp = currentTemp;
    String csv = "";

    if (running) {
      unsigned long t = millis() - startTime;
      int totalTime = getTotalTime();

      targetTemp = getTargetTemp(t);
      timeLeft = totalTime - (t / 1000);

      csv = String((int)(t/1000)) + "," +
            String(currentTemp) + "," +
            String(targetTemp);
    }

    Serial.println("---- UPLOAD ----");
    Serial.println("TEMP: " + String(currentTemp));
    Serial.println("TIME LEFT: " + String(timeLeft));
    Serial.println("CSV: " + csv);

    sendData(csv, currentTemp, timeLeft);
  }
}
