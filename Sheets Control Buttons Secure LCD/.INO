#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <max6675.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include "mbedtls/md.h"

// =====================
// PINS
// =====================
#define LCD_SDA_PIN     21
#define LCD_SCL_PIN     22
#define MAX6675_CS       5
#define MAX6675_SCK     18
#define MAX6675_SO      19
#define START_BUTTON     4
#define STOP_BUTTON     17
#define SSR_PIN         32
#define BUZZER_PIN      26

// =====================
// WIFI
// =====================
#define WIFI_SSID          "DPhone"
#define WIFI_PASSWORD      "12345678"
#define WIFI_TIMEOUT_MS    20000   // give up after 20 seconds

// =====================
// SCRIPT
// =====================
#define SCRIPT_URL "https://script.google.com/macros/s/AKfycbwfbBBiYX8Zja1W6BHC4lEwvNKXgq-bvKIUp_X2Ix2JB1eDEOAVtVT3enoVKnchQYQ66g/exec"
#define DEVICE_ID  "oven_1"

// =====================
// SECURITY
// =====================
String API_KEY = "u2DTMufqSvkij72zo6uCY4dSemA3UPdgxngLauKrh5HYfSECP5CB6zkrB9Zi"; // change for each oven
String SECRET  = "6YGbkpqMQj8zAav9hkmMMb4rGSAoJazG75aXrh3mrpZhhVS9XXZ8rysfUajp"; // keep the same

// =====================
// SERIAL
// =====================
#define SERIAL_BAUD 9600

// =====================
// TIME
// =====================
const char* NTP_SERVER_1        = "pool.ntp.org";
const char* NTP_SERVER_2        = "time.nist.gov";
const long  GMT_OFFSET_SEC      = 12 * 3600;
const int   DAYLIGHT_OFFSET_SEC = 0;

// =====================
// OBJECTS
// =====================
LiquidCrystal_I2C lcd(0x27, 16, 2);
MAX6675           thermocouple(MAX6675_SCK, MAX6675_CS, MAX6675_SO);
WiFiClientSecure  secureClient;

// =====================
// PROFILE STRUCTURE
// =====================
struct Step {
  float temp;
  int   duration;
  char  stageName[64];
};

struct DutyCycleBand {
  float tempDelta;
  float dutyCycle;
};

Step          profile[8];
DutyCycleBand dutyCycleBands[12];

int NUM_STEPS    = 0;
int NUM_DC_BANDS = 0;

int    currentStepIndex = 0;
String currentStageName = "";

// =====================
// STATE
// =====================
bool          running       = false;
unsigned long startTime     = 0;
unsigned long lastControl   = 0;
unsigned long lastUpload    = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastLCD       = 0;
bool          ssrState      = false;
bool          wifiConnected = false;
bool          offlineMode   = false;  // true when WiFi never connected at boot
int           activeDefaultProfile = 1; // which default profile is selected offline

// LCD display globals (written by control loop, read by updateLCD)
float lcdCurrentTemp = 0;
float lcdTargetTemp  = 0;
int   lcdTimeLeft    = 0;

// =====================
// BUTTON DEBOUNCE
// =====================
#define DEBOUNCE_DELAY 50
unsigned long lastRunButtonTime  = 0;
unsigned long lastStopButtonTime = 0;
bool lastRunButtonState  = HIGH;
bool lastStopButtonState = HIGH;

bool   runLocked     = false;
String lastRemoteCmd = "";

// =====================
// BUZZER (NON-BLOCKING)
// =====================
unsigned long beepEnd = 0;

void beep(int freq, int durationMs) {
  ledcWriteTone(0, freq);
  beepEnd = millis() + durationMs;
}

// =====================
// LCD HELPERS
// =====================

// Print a 16-char padded line to the LCD (pads/truncates to exactly 16 chars)
void lcdLine(int row, String msg) {
  while (msg.length() < 16) msg += ' ';
  msg = msg.substring(0, 16);
  lcd.setCursor(0, row);
  lcd.print(msg);
}

// Show a two-line error message and hold for durationMs
void lcdError(String line1, String line2, int durationMs = 2000) {
  lcdLine(0, "ERR:" + line1);
  lcdLine(1, line2);
  Serial.println("LCD ERR: " + line1 + " | " + line2);
  delay(durationMs);
}

// =====================
// SPIFFS
// =====================
#define CONFIG_FILE "/config.json"

void initSPIFFS() {
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
    lcdError("SPIFFS", "Mount failed");
    return;
  }
  Serial.println("SPIFFS Mounted");
}

void saveConfigToFlash() {
  JsonDocument doc;

  JsonArray stepsArray = doc.createNestedArray("profile");
  for (int i = 0; i < NUM_STEPS; i++) {
    JsonObject step   = stepsArray.createNestedObject();
    step["temp"]      = profile[i].temp;
    step["duration"]  = profile[i].duration;
    step["stageName"] = profile[i].stageName;
  }

  JsonArray bandsArray = doc.createNestedArray("dutyCycles");
  for (int i = 0; i < NUM_DC_BANDS; i++) {
    JsonObject band    = bandsArray.createNestedObject();
    band["tempDelta"]  = dutyCycleBands[i].tempDelta;
    band["dutyCycle"]  = dutyCycleBands[i].dutyCycle;
  }

  String newConfig;
  serializeJson(doc, newConfig);

  if (SPIFFS.exists(CONFIG_FILE)) {
    File f = SPIFFS.open(CONFIG_FILE, "r");
    String oldConfig = f.readString();
    f.close();
    if (oldConfig == newConfig) {
      Serial.println("Config unchanged - skip flash write");
      return;
    }
  }

  File configFile = SPIFFS.open(CONFIG_FILE, "w");
  if (!configFile) {
    Serial.println("Failed to open config file for writing");
    lcdError("FLASH", "Write failed");
    return;
  }
  configFile.print(newConfig);
  configFile.close();
  Serial.println("Config saved to flash");
}

bool loadConfigFromFlash() {
  if (!SPIFFS.exists(CONFIG_FILE)) {
    Serial.println("Config file not found on flash");
    return false;
  }

  File configFile = SPIFFS.open(CONFIG_FILE, "r");
  if (!configFile) {
    Serial.println("Failed to open config file");
    lcdError("FLASH", "Open failed");
    return false;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();

  if (error) {
    Serial.println("Failed to parse config JSON: " + String(error.c_str()));
    lcdError("FLASH", "Bad JSON");
    return false;
  }

  JsonArray stepsArray = doc["profile"];
  NUM_STEPS = min((int)stepsArray.size(), 8);
  for (int i = 0; i < NUM_STEPS; i++) {
    profile[i].temp     = stepsArray[i]["temp"];
    profile[i].duration = stepsArray[i]["duration"];
    strncpy(profile[i].stageName, stepsArray[i]["stageName"].as<const char*>(), 63);
    profile[i].stageName[63] = '\0';
  }

  JsonArray bandsArray = doc["dutyCycles"];
  NUM_DC_BANDS = min((int)bandsArray.size(), 12);
  for (int i = 0; i < NUM_DC_BANDS; i++) {
    dutyCycleBands[i].tempDelta = bandsArray[i]["tempDelta"];
    dutyCycleBands[i].dutyCycle = bandsArray[i]["dutyCycle"];
  }

  Serial.println("Config loaded from flash (" + String(NUM_STEPS) + " steps)");
  return true;
}

void printConfig() {
  Serial.println("\n===== CURRENT CONFIG =====");
  for (int i = 0; i < NUM_STEPS; i++) {
    Serial.println("  Step " + String(i) + ": " + String(profile[i].temp) +
                   "C for " + String(profile[i].duration) + "s [" +
                   String(profile[i].stageName) + "]");
  }
  for (int i = 0; i < NUM_DC_BANDS; i++) {
    Serial.println("  Band " + String(i) + ": dT=" +
                   String(dutyCycleBands[i].tempDelta) + " DC=" +
                   String(dutyCycleBands[i].dutyCycle) + "%");
  }
  Serial.println("==========================\n");
}

// =====================
// DEFAULT PROFILES
// =====================
static const DutyCycleBand DEFAULT_BANDS[6] = {
  {  5.0f, 100.0f },
  {  3.0f,  80.0f },
  {  1.0f,  50.0f },
  {  0.0f,  20.0f },
  { -1.0f,  10.0f },
  { -99.0f,  0.0f }
};
#define DEFAULT_NUM_BANDS 6

// Profile 0 — test/calibration: 15->40->5 over 15 min
static const Step DEFAULT_STEPS_TEST[3] = {
  { 40.0f,  300, "Ramp Up"   },
  { 40.0f,  600, "Soak"      },
  {  5.0f,  300, "Cool Down" }
};

// Profile 1 — OOA prepreg carbon fibre cure
static const Step DEFAULT_STEPS_CARBON[5] = {
  {  70.0f,  3000, "Ramp to Flow" },
  {  70.0f,  3600, "Flow Soak"    },
  { 125.0f,  3300, "Ramp to Cure" },
  { 125.0f,  5400, "Cure Soak"    },
  {  40.0f,  9900, "Cool Down"    }
};

const char* DEFAULT_PROFILE_NAMES[2] = {
  "Test Cycle",
  "Carbon Fibre"
};

void loadDefaultProfile(int profileIndex) {
  activeDefaultProfile = profileIndex;
  if (profileIndex == 0) {
    NUM_STEPS = 3;
    for (int i = 0; i < NUM_STEPS; i++) profile[i] = DEFAULT_STEPS_TEST[i];
    Serial.println("DEFAULT PROFILE: Test Cycle");
  } else {
    NUM_STEPS = 5;
    for (int i = 0; i < NUM_STEPS; i++) profile[i] = DEFAULT_STEPS_CARBON[i];
    Serial.println("DEFAULT PROFILE: Carbon Fibre Cure");
  }
  NUM_DC_BANDS = DEFAULT_NUM_BANDS;
  for (int i = 0; i < NUM_DC_BANDS; i++) dutyCycleBands[i] = DEFAULT_BANDS[i];
  printConfig();
}

// =====================
// OUTPUT CONTROL
// =====================
void setSSR(bool state) {
  ssrState = state;
  digitalWrite(SSR_PIN, state);
}

// =====================
// LCD UPDATE
// =====================
void updateLCD() {
  static float idleTemp     = 0;
  static unsigned long lastIdleRead = 0;

  if (!running) {
    // Refresh idle temp at 1 Hz
    if (millis() - lastIdleRead >= 1000) {
      lastIdleRead = millis();
      idleTemp = thermocouple.readCelsius();
    }

    // Line 0: device ID + wifi status
    String statusChar = wifiConnected ? "W" : (offlineMode ? "X" : "?");
    lcdLine(0, String(DEVICE_ID) + "  " + statusChar + "  " + String(idleTemp, 1) + "C");

    // Line 1: selected profile name (offline) or IDLE (online)
    if (offlineMode) {
      lcdLine(1, String(DEFAULT_PROFILE_NAMES[activeDefaultProfile]));
    } else if (NUM_STEPS == 0) {
      lcdLine(1, "NO PROFILE");
    } else {
      lcdLine(1, "IDLE");
    }
    return;
  }

  // Running — line 0: temps + SSR indicator
  char ssrChar = ssrState ? '*' : '-';
  char line1[17];
  snprintf(line1, sizeof(line1), "C:%-5.1f T:%-4.1f%c",
           lcdCurrentTemp, lcdTargetTemp, ssrChar);
  lcd.setCursor(0, 0);
  lcd.print(line1);

  // Running — line 1: stage name + MM:SS remaining
  char stageBuf[12];
  strncpy(stageBuf, currentStageName.c_str(), 11);
  stageBuf[11] = '\0';
  int mins = lcdTimeLeft / 60;
  int secs = lcdTimeLeft % 60;
  char line2[17];
  snprintf(line2, sizeof(line2), "%-11s%02d:%02d", stageBuf, mins, secs);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

// =====================
// BUTTON DEBOUNCE
// =====================
bool readButton(int pin, unsigned long &lastTime, bool &lastState) {
  bool currentState = digitalRead(pin);
  if (currentState != lastState) {
    lastTime  = millis();
    lastState = currentState;
    return false;
  }
  if ((millis() - lastTime) > DEBOUNCE_DELAY) {
    return currentState == LOW;
  }
  return false;
}

// =====================
// SECURITY
// =====================
String createHMAC(String message) {
  byte hmacResult[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)SECRET.c_str(), SECRET.length());
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)message.c_str(), message.length());
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);
  String hash = "";
  for (int i = 0; i < 32; i++) {
    if (hmacResult[i] < 16) hash += "0";
    hash += String(hmacResult[i], HEX);
  }
  hash.toLowerCase();
  return hash;
}

bool timeIsSynced() {
  return time(nullptr) > 1700000000;
}

long long getEpochMillis() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return ((long long)tv.tv_sec * 1000LL) + (tv.tv_usec / 1000);
}

void syncTime() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
  Serial.print("SYNCING TIME");
  int tries = 0;
  while (!timeIsSynced() && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  if (timeIsSynced()) {
    Serial.println("\nTIME SYNCED");
  } else {
    Serial.println("\nTIME SYNC FAILED");
    lcdError("TIME", "Sync failed", 1500);
  }
}

// =====================
// TARGET TEMP
// =====================
float getTargetTemp(unsigned long t) {
  unsigned long elapsed  = t / 1000;
  int           cumulative = 0;

  for (int i = 0; i < NUM_STEPS; i++) {
    float startTemp = (i == 0) ? profile[0].temp : profile[i - 1].temp;
    float endTemp   = profile[i].temp;

    if (elapsed < (unsigned long)(cumulative + profile[i].duration)) {
      currentStepIndex = i;
      currentStageName = String(profile[i].stageName);
      float progress   = float(elapsed - cumulative) / profile[i].duration;
      return startTemp + progress * (endTemp - startTemp);
    }
    cumulative += profile[i].duration;
  }

  currentStepIndex = NUM_STEPS - 1;
  currentStageName = String(profile[NUM_STEPS - 1].stageName);
  return profile[NUM_STEPS - 1].temp;
}

int getTotalTime() {
  int total = 0;
  for (int i = 0; i < NUM_STEPS; i++) total += profile[i].duration;
  return total;
}

// =====================
// DUTY CYCLE LOOKUP
// =====================
float getDutyCycle(float currentTemp, float targetTemp) {
  float error = targetTemp - currentTemp;
  for (int i = 0; i < NUM_DC_BANDS; i++) {
    if (error <= dutyCycleBands[i].tempDelta) {
      return dutyCycleBands[i].dutyCycle / 100.0;
    }
  }
  return (NUM_DC_BANDS > 0) ? dutyCycleBands[NUM_DC_BANDS - 1].dutyCycle / 100.0 : 0.2;
}

// =====================
// SSR CONTROL
// =====================
void controlSSR(float currentTemp, float targetTemp) {
  if (currentTemp >= targetTemp + 1.0) {
    setSSR(false);
    Serial.println("SSR OFF (over +1C)");
    return;
  }
  float duty   = getDutyCycle(currentTemp, targetTemp);
  int   onTime = duty * 1000;
  if (millis() % 1000 < (unsigned long)onTime) {
    setSSR(true);
    Serial.println("SSR ON");
  } else {
    setSSR(false);
    Serial.println("SSR OFF (PWM)");
  }
  Serial.println("ERROR: " + String(targetTemp - currentTemp));
  Serial.println("DUTY:  " + String(duty));
}

// =====================
// FETCH SETTINGS
// =====================
bool fetchSettingsFromGoogle() {
  if (!wifiConnected) return false;
  if (!timeIsSynced())  return false;

  HTTPClient http;
  String url = String(SCRIPT_URL) +
    "?type=settings&device=" + String(DEVICE_ID) + "&key=" + API_KEY;

  Serial.println("FETCHING SETTINGS");
  secureClient.setInsecure();
  http.begin(secureClient, url);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(10000);

  int code = http.GET();
  Serial.println("SETTINGS HTTP: " + String(code));

  if (code != 200) {
    http.end();
    String errMsg = "HTTP " + String(code);
    lcdError("SHEET", errMsg, 2000);
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.println("Settings JSON parse error: " + String(err.c_str()));
    lcdError("SHEET", "Bad JSON", 2000);
    return false;
  }

  JsonArray stepsArray = doc["profile"];
  NUM_STEPS = min((int)stepsArray.size(), 8);
  for (int i = 0; i < NUM_STEPS; i++) {
    profile[i].temp     = stepsArray[i]["temp"];
    profile[i].duration = stepsArray[i]["duration"];
    strncpy(profile[i].stageName, stepsArray[i]["stageName"].as<const char*>(), 63);
    profile[i].stageName[63] = '\0';
  }

  JsonArray bandsArray = doc["dutyCycles"];
  NUM_DC_BANDS = min((int)bandsArray.size(), 12);
  for (int i = 0; i < NUM_DC_BANDS; i++) {
    dutyCycleBands[i].tempDelta = bandsArray[i]["tempDelta"];
    dutyCycleBands[i].dutyCycle = bandsArray[i]["dutyCycle"];
  }

  Serial.println("SETTINGS LOADED FROM SHEET");
  saveConfigToFlash();
  printConfig();
  return true;
}

// =====================
// BUTTON EVENT
// =====================
void sendButtonEvent(String command) {
  if (!wifiConnected) return;
  HTTPClient http;
  String url = String(SCRIPT_URL) +
    "?type=button&device=" + String(DEVICE_ID) +
    "&command=" + command + "&key=" + API_KEY +
    "&t=" + String(millis());
  secureClient.setInsecure();
  http.begin(secureClient, url);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(5000);
  int code = http.GET();
  Serial.println("BUTTON EVENT: " + String(code));
  http.end();
}

// =====================
// URL ENCODE
// =====================
String urlEncode(String str) {
  String encoded = "";
  for (int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if      (c == ' ')  encoded += "%20";
    else if (c == '&')  encoded += "%26";
    else if (c == '=')  encoded += "%3D";
    else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
             (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
      encoded += c;
    else {
      encoded += "%";
      if (c < 16) encoded += "0";
      encoded += String(c, HEX);
    }
  }
  return encoded;
}

// =====================
// SEND DATA
// =====================
void sendData(String csv, float temp, int timeLeft) {
  if (!wifiConnected || !timeIsSynced()) return;

  HTTPClient http;
  long long ts       = getEpochMillis();
  String tempStr     = String(temp);
  String timeLeftStr = String(timeLeft);
  String stageStr    = urlEncode(currentStageName);
  String payload     = csv + tempStr + timeLeftStr + String(ts);
  String sig         = createHMAC(payload);

  String url = String(SCRIPT_URL) +
    "?type=log&device=" + String(DEVICE_ID) +
    "&csv=" + csv + "&temp=" + tempStr +
    "&timeleft=" + timeLeftStr + "&stageName=" + stageStr +
    "&key=" + API_KEY + "&ts=" + String(ts) + "&sig=" + sig;

  secureClient.setInsecure();
  http.begin(secureClient, url);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(5000);

  int code = http.GET();
  Serial.println("SEND HTTP: " + String(code));
  if (code > 0) {
    String resp = http.getString();
    Serial.println("RESPONSE: " + resp);
    if (resp != "OK" && resp != "NO_DATA") {
      Serial.println("SEND WARNING: unexpected response");
    }
  } else {
    Serial.println("SEND FAILED");
  }
  http.end();
}

// =====================
// GET COMMAND
// =====================
String getCommand() {
  if (!wifiConnected) return "";
  HTTPClient http;
  String url = String(SCRIPT_URL) +
    "?type=control&device=" + String(DEVICE_ID) +
    "&key=" + API_KEY + "&t=" + String(millis());
  secureClient.setInsecure();
  http.begin(secureClient, url);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(5000);
  int code = http.GET();
  Serial.println("CMD HTTP: " + String(code));
  if (code > 0) {
    String payload = http.getString();
    payload.trim();
    http.end();
    return payload;
  }
  http.end();
  return "";
}

// =====================
// SETUP
// =====================
void setup() {
  Serial.begin(SERIAL_BAUD);

  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  lcd.init();
  lcd.backlight();

  pinMode(SSR_PIN,      OUTPUT);
  pinMode(START_BUTTON, INPUT_PULLUP);
  pinMode(STOP_BUTTON,  INPUT_PULLUP);

  // ESP32 core v3.x: ledcSetup/ledcAttachPin replaced by ledcAttach
  ledcAttach(BUZZER_PIN, 0, 8);  // channel 0, 8-bit resolution

  setSSR(false);

  Serial.println("\n==================");
  Serial.println("BOOTING " + String(DEVICE_ID));
  Serial.println("==================");

  // ---- Startup splash ----
  lcdLine(0, String(DEVICE_ID));
  lcdLine(1, "BOOTING...");

  initSPIFFS();

  // ---- WiFi with 20s timeout ----
  lcdLine(0, String(DEVICE_ID) + " WIFI...");
  lcdLine(1, "");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long wifiStart = millis();
  int dotCol = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < WIFI_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
    // Print progress dots on line 1
    lcd.setCursor(dotCol, 1);
    lcd.print(".");
    dotCol = (dotCol + 1) % 16;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    // ---- WiFi connected ----
    wifiConnected = true;
    offlineMode   = false;
    String ip     = WiFi.localIP().toString();
    Serial.println("WIFI OK: " + ip);

    // Show device + IP for 2.5 seconds
    lcdLine(0, String(DEVICE_ID) + " ONLINE");
    lcdLine(1, ip);
    delay(2500);

    // ---- Time sync ----
    lcdLine(0, String(DEVICE_ID));
    lcdLine(1, "TIME SYNC...");
    syncTime();

    // ---- Fetch profile from sheet ----
    lcdLine(1, "FETCHING CFG...");
    if (fetchSettingsFromGoogle()) {
      Serial.println("Using Google Sheet settings");
      lcdLine(1, "SHEET OK");
      delay(800);
    } else {
      // fetchSettingsFromGoogle already showed an lcdError
      lcdLine(1, "TRYING FLASH...");
      delay(600);
      if (loadConfigFromFlash()) {
        Serial.println("Using cached flash settings");
        lcdLine(1, "FLASH OK");
        delay(800);
      } else {
        Serial.println("No flash config - loading default");
        lcdError("NO PROFILE", "Using default", 1500);
        loadDefaultProfile(activeDefaultProfile);
      }
    }

  } else {
    // ---- WiFi failed ----
    wifiConnected = false;
    offlineMode   = true;
    Serial.println("WIFI FAILED - OFFLINE");

    lcdLine(0, String(DEVICE_ID) + " OFFLINE");
    lcdLine(1, "NO WIFI");
    delay(1000);

    lcdError("WIFI", "No connection", 2000);

    // Try flash cache first
    lcdLine(1, "TRYING FLASH...");
    delay(400);
    if (loadConfigFromFlash()) {
      Serial.println("Using cached flash settings");
      lcdLine(1, "FLASH OK");
      delay(800);
    } else {
      Serial.println("No flash config - loading default");
      lcdError("NO FLASH", "Using default", 1500);
      loadDefaultProfile(activeDefaultProfile);
    }
  }

  printConfig();

  // ---- Ready ----
  lcdLine(0, String(DEVICE_ID) + (offlineMode ? " OFFLINE" : " ONLINE"));
  lcdLine(1, offlineMode
    ? String("P:") + DEFAULT_PROFILE_NAMES[activeDefaultProfile]
    : "READY");

  beep(1000, 200);
  delay(300);
  beep(1500, 100);
}

// =====================
// LOOP
// =====================
void loop() {
  unsigned long now = millis();

  // ---- Stop beep cleanly ----
  if (beepEnd && now > beepEnd) {
    ledcWriteTone(0, 0);
    beepEnd = 0;
  }

  // ---- WiFi keep-alive (only if not in hard offline mode) ----
  if (!offlineMode && now - lastWiFiCheck > 10000) {
    lastWiFiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      Serial.println("WiFi lost - reconnecting...");
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    } else {
      wifiConnected = true;
      if (!timeIsSynced()) syncTime();
    }
  }

  // ---- Buttons ----
  bool runPressed  = readButton(START_BUTTON, lastRunButtonTime,  lastRunButtonState);
  bool stopPressed = readButton(STOP_BUTTON,  lastStopButtonTime, lastStopButtonState);

  // ---- STOP: cycle profiles when offline and idle; otherwise stop run ----
  if (stopPressed) {
    if (running) {
      running   = false;
      runLocked = false;
      setSSR(false);
      Serial.println("STOPPED (LOCAL)");
      beep(400, 400);
      if (wifiConnected) sendButtonEvent("STOP");
    } else if (offlineMode) {
      // Cycle to next default profile
      activeDefaultProfile = (activeDefaultProfile + 1) % 2;
      loadDefaultProfile(activeDefaultProfile);
      Serial.println("OFFLINE PROFILE: " + String(DEFAULT_PROFILE_NAMES[activeDefaultProfile]));
      beep(800, 80);
      delay(120);
      beep(1100, 80);
      lcdLine(0, String(DEVICE_ID) + " OFFLINE");
      lcdLine(1, String("P:") + DEFAULT_PROFILE_NAMES[activeDefaultProfile]);
    }
  }

  // ---- START button ----
  if (runPressed) {
    Serial.println("RUN BUTTON PRESSED");
    if (!running && !runLocked) {
      running   = true;
      startTime = now;
      Serial.println("RUN STARTED (LOCAL)");
      beep(1200, 150);
      if (wifiConnected) sendButtonEvent("RUN");
    } else if (runLocked) {
      Serial.println("RUN LOCKED");
    }
  }

  // ---- Remote command (every ~10s, online only) ----
  if (wifiConnected && now % 10000 < 100) {
    String cmd  = getCommand();
    bool newCmd = (cmd != lastRemoteCmd);
    lastRemoteCmd = cmd;

    if (cmd == "RUN" && !running && newCmd) {
      running   = true;
      startTime = now;
      Serial.println("RUN STARTED (REMOTE)");
      beep(1200, 150);
    } else if (cmd == "STOP" && (running || newCmd)) {
      running   = false;
      runLocked = false;
      setSSR(false);
      Serial.println("STOPPED (REMOTE)");
      beep(400, 400);
    }
  }

  // ---- Control loop (1s) ----
  if (running && now - lastControl >= 1000) {
    lastControl = now;

    unsigned long t        = now - startTime;
    int           totalTime = getTotalTime();
    float currentTemp      = thermocouple.readCelsius();
    float targetTemp       = getTargetTemp(t);
    int   timeLeft         = totalTime - (int)(t / 1000);

    lcdCurrentTemp = currentTemp;
    lcdTargetTemp  = targetTemp;
    lcdTimeLeft    = timeLeft;

    Serial.println("---- CONTROL ----");
    Serial.println("TIME:    " + String(t / 1000));
    Serial.println("CURRENT: " + String(currentTemp));
    Serial.println("TARGET:  " + String(targetTemp));

    // ---- Profile complete ----
    if ((int)(t / 1000) >= totalTime) {
      static int finishSendCount = 0;

      float finalTemp   = thermocouple.readCelsius();
      float finalTarget = profile[NUM_STEPS - 1].temp;

      setSSR(false);
      running          = false;
      runLocked        = false;
      currentStageName = "Complete";
      lcdCurrentTemp   = finalTemp;
      lcdTargetTemp    = finalTarget;
      lcdTimeLeft      = 0;

      String finalCsv = String(totalTime) + "," +
                        String(finalTemp)  + "," +
                        String(finalTarget);

      if (finishSendCount < 5) {
        sendData(finalCsv, finalTemp, 0);
        finishSendCount++;
      }

      Serial.println("PROFILE COMPLETE");
      beep(800,  200); delay(250);
      beep(1200, 200); delay(250);
      beep(1800, 400);

      lcdLine(0, "COMPLETE");
      lcdLine(1, String(DEVICE_ID) + (offlineMode ? " OFFLINE" : " ONLINE"));
      return;
    }

    controlSSR(currentTemp, targetTemp);
  }

  // ---- Upload every 20s (online only) ----
  if (wifiConnected && now - lastUpload >= 20000) {
    lastUpload = now;

    float  currentTemp = thermocouple.readCelsius();
    int    timeLeft    = 0;
    float  targetTemp  = currentTemp;
    String csv         = "";

    if (running) {
      unsigned long t = now - startTime;
      targetTemp = getTargetTemp(t);
      timeLeft   = getTotalTime() - (int)(t / 1000);
      csv = String((int)(t / 1000)) + "," +
            String(currentTemp) + "," +
            String(targetTemp);
    }

    Serial.println("---- UPLOAD ----");
    Serial.println("TEMP: " + String(currentTemp) + "  LEFT: " + String(timeLeft));
    sendData(csv, currentTemp, timeLeft);
  }

  // ---- LCD update (200ms) ----
  if (now - lastLCD >= 200) {
    lastLCD = now;
    updateLCD();
  }
}
