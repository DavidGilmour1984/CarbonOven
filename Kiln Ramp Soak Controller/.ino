#include "max6675.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

int thermoDO   = 11;
int thermoCS   = 10;
int thermoCLK  = 9;
int startPin   = 12;
int elementPin = 13;
int profilePin = 2;

MAX6675 thermocouple(thermoCLK, thermoCS, thermoDO);
LiquidCrystal_I2C lcd(0x27, 20, 4);

// ── Profiles ──────────────────────────────────────────────────────────────────

struct Stage {
  const char*   name;
  float         targetTemp;
  unsigned long durationSec;
};

// Profile 1: PLA Burnout Wet
Stage wetStages[] = {
  { "Dry Ramp", 120,  3600 },
  { "Dry Soak", 120,  3600 },
  { "Stm Ramp", 230,  1800 },
  { "Stm Soak", 230,  3600 },
  { "Mlt Ramp", 350,  1800 },
  { "Mlt Soak", 350,  3600 },
  { "Brn Ramp", 550,  1800 },
  { "Burnout ", 550,  3600 }
};
const int wetStageCount = 8;

// Profile 2: PLA Burnout Dry
Stage dryStages[] = {
  { "Stm Ramp", 230,  1800 },
  { "Stm Soak", 230,  1800 },
  { "Mlt Ramp", 350,  1200 },
  { "Mlt Soak", 350,  2700 },
  { "Brn Ramp", 550,  1200 },
  { "Burnout ", 550,  2700 }
};
const int dryStageCount = 6;

// Profile 3: Aluminium Melt
Stage alStages[] = {
  { "Preheat ", 400,  1200 },
  { "Melting ", 720,  1800 },
  { "PourTemp", 720,     0 }
};
const int alStageCount = 3;

// Profile 4: Pewter Melt
Stage pewterStages[] = {
  { "Melting ", 320,   600 },
  { "PourTemp", 320,     0 }
};
const int pewterStageCount = 2;

const char* profileNames[] = {
  "PLA Wet Burnout",
  "PLA Dry Burnout",
  "Aluminium Melt ",
  "Pewter Melt    ",
  "Manual Mode    "
};

Stage* profiles[]    = { wetStages,     dryStages,     alStages,     pewterStages     };
int    stageCounts[] = { wetStageCount,  dryStageCount,  alStageCount, pewterStageCount };

const int profileCount   = 5;  // 4 auto + 1 manual
int       currentProfile = 0;

// ── Runtime state ─────────────────────────────────────────────────────────────

unsigned long startTime      = 0;
int           currentStage   = 0;
float         stageStartTemp = 0;
float         stageStartTime = 0;
float         targetTemp     = 0;

// ── Manual mode state ─────────────────────────────────────────────────────────

float         manualTarget   = 0;
bool          lastStartBtn   = HIGH;
bool          lastF1Btn      = HIGH;
const float   manualStep     = 5.0;
const float   manualMax      = 750.0;
const float   manualMin      = 0.0;

// ── Helpers ───────────────────────────────────────────────────────────────────

void showPinout(int page) {
  lcd.clear();
  if (page == 0) {
    lcd.setCursor(0, 0); lcd.print("---- Pinout 1/2 ----");
    lcd.setCursor(0, 1); lcd.print("D2  : F1 Button");
    lcd.setCursor(0, 2); lcd.print("D9  : MAX6675 CLK");
    lcd.setCursor(0, 3); lcd.print("D10 : MAX6675 CS");
  } else {
    lcd.setCursor(0, 0); lcd.print("---- Pinout 2/2 ----");
    lcd.setCursor(0, 1); lcd.print("D11 : MAX6675 DO");
    lcd.setCursor(0, 2); lcd.print("D12 : Start Button");
    lcd.setCursor(0, 3); lcd.print("D13 : Element");
  }
}

void showProfileScreen() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("F1 To Change Cycle");
  for (int i = 0; i < 3; i++) {
    int idx = (currentProfile + i) % profileCount;
    lcd.setCursor(0, i + 1);
    lcd.print(i == 0 ? "> " : "  ");
    lcd.print(profileNames[idx]);
  }
}

unsigned long totalProfileTime(int p) {
  if (p >= 4) return 0;  // manual has no fixed time
  unsigned long t = 0;
  for (int i = 0; i < stageCounts[p]; i++) t += profiles[p][i].durationSec;
  return t;
}

void printHMS(unsigned long totalSec) {
  int h = totalSec / 3600;
  int m = (totalSec % 3600) / 60;
  int s = totalSec % 60;
  if (h < 10) lcd.print("0");
  lcd.print(h);
  lcd.print("h ");
  if (m < 10) lcd.print("0");
  lcd.print(m);
  lcd.print("m ");
  if (s < 10) lcd.print("0");
  lcd.print(s);
  lcd.print("s");
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
  pinMode(startPin,   INPUT);
  pinMode(elementPin, OUTPUT);
  pinMode(profilePin, INPUT_PULLUP);
  Serial.begin(9600);
  delay(500);

  lcd.init();
  lcd.backlight();
  showProfileScreen();

  int  menuPage    = 0;
  int  aboutPage   = 0;
  bool lastBtn     = HIGH;
  bool lastStart   = LOW;
  unsigned long lastPageFlip  = 0;
  const unsigned long flipInterval = 5000;

  while (true) {
    bool btn   = digitalRead(profilePin);
    bool start = digitalRead(startPin);

    // ── F1 pressed ────────────────────────────────────────────────────────────
    if (btn == LOW && lastBtn == HIGH) {
      delay(50);
      if (menuPage == 0) {
        currentProfile = (currentProfile + 1) % profileCount;
        if (currentProfile == 0) {
          menuPage     = 1;
          aboutPage    = 0;
          lastPageFlip = millis();
          showPinout(aboutPage);
        } else {
          showProfileScreen();
        }
      } else {
        menuPage = 0;
        showProfileScreen();
      }
    }
    lastBtn = btn;

    // ── Start pressed ─────────────────────────────────────────────────────────
    if (start == HIGH && lastStart == LOW) {
      delay(50);
      if (menuPage == 1) {
        menuPage = 0;
        showProfileScreen();
      } else {
        break;
      }
    }
    lastStart = start;

    // ── About page auto-flip every 5 seconds ──────────────────────────────────
    if (menuPage == 1 && millis() - lastPageFlip >= flipInterval) {
      aboutPage    = 1 - aboutPage;
      lastPageFlip = millis();
      showPinout(aboutPage);
    }

    delay(10);
  }

  // ── Start run ─────────────────────────────────────────────────────────────
  float initTemp   = thermocouple.readCelsius();
  stageStartTemp   = initTemp;
  stageStartTime   = 0;
  currentStage     = 0;
  startTime        = millis();

  // Manual mode: start target at current measured temp
  if (currentProfile == 4) {
    manualTarget = initTemp;
    lastStartBtn = digitalRead(startPin);
    lastF1Btn    = digitalRead(profilePin);
  }

  Serial.print("Profile: ");
  Serial.println(profileNames[currentProfile]);
}

// ── Loop ──────────────────────────────────────────────────────────────────────

void loop() {
  float temperature = thermocouple.readCelsius();
  if (temperature == 0) {
    Serial.println("Error: check thermocouple.");
    return;
  }

  unsigned long elapsed = (millis() - startTime) / 1000;

  // ════════════════════════════════════════════════════════════════════════════
  // MANUAL MODE
  // ════════════════════════════════════════════════════════════════════════════
  if (currentProfile == 4) {
    bool startBtn = digitalRead(startPin);
    bool f1Btn    = digitalRead(profilePin);

    // Start button → increase target by 5°C
    if (startBtn == HIGH && lastStartBtn == LOW) {
      delay(30);
      manualTarget += manualStep;
      if (manualTarget > manualMax) manualTarget = manualMax;
    }
    lastStartBtn = startBtn;

    // F1 button → decrease target by 5°C
    if (f1Btn == LOW && lastF1Btn == HIGH) {
      delay(30);
      manualTarget -= manualStep;
      if (manualTarget < manualMin) manualTarget = manualMin;
    }
    lastF1Btn = f1Btn;

    targetTemp = manualTarget;

    // Element control
    if (temperature < (targetTemp - 8)) {
      digitalWrite(elementPin, HIGH);
    } else if (temperature <= targetTemp) {
      digitalWrite(elementPin, (millis() % 1000 < 300) ? HIGH : LOW);
    } else if (temperature <= (targetTemp + 5)) {
      digitalWrite(elementPin, (millis() % 2000 < 200) ? HIGH : LOW);
    } else {
      digitalWrite(elementPin, LOW);
    }

    // Serial
    Serial.print(temperature, 2);
    Serial.print(" ");
    Serial.println(targetTemp, 2);

    // LCD
    lcd.setCursor(0, 0);
    lcd.print("Mode   : Manual     ");

    lcd.setCursor(0, 1);
    lcd.print("Time   : ");
    printHMS(elapsed);

    lcd.setCursor(0, 2);
    lcd.print("Target : ");
    lcd.print(targetTemp, 0);
    lcd.print("C +Strt ");

    lcd.setCursor(0, 3);
    lcd.print("Actual : ");
    lcd.print(temperature, 0);
    lcd.print("C -F1   ");

    delay(100);
    return;
  }

  // ════════════════════════════════════════════════════════════════════════════
  // AUTO PROFILE MODES
  // ════════════════════════════════════════════════════════════════════════════

  Stage*        stages    = profiles[currentProfile];
  int           numStages = stageCounts[currentProfile];

  // ── Advance stage ──────────────────────────────────────────────────────────
  while (currentStage < numStages - 1 &&
         stages[currentStage].durationSec > 0 &&
         elapsed >= (stageStartTime + stages[currentStage].durationSec)) {
    stageStartTime += stages[currentStage].durationSec;
    currentStage++;
    stageStartTemp = stages[currentStage - 1].targetTemp;
    Serial.print("Stage: ");
    Serial.println(stages[currentStage].name);
  }

  // ── Target temp ────────────────────────────────────────────────────────────
  Stage& s      = stages[currentStage];
  bool   isRamp = (s.durationSec > 0) && (s.targetTemp != stageStartTemp);
  if (isRamp) {
    float rate         = (s.targetTemp - stageStartTemp) / (float)s.durationSec;
    float stageElapsed = elapsed - stageStartTime;
    targetTemp = stageStartTemp + rate * stageElapsed;
    if (targetTemp > s.targetTemp) targetTemp = s.targetTemp;
  } else {
    targetTemp = s.targetTemp;
  }

  // ── Completion ─────────────────────────────────────────────────────────────
  bool complete = (stages[numStages - 1].durationSec > 0 &&
                   currentStage == numStages - 1 &&
                   elapsed >= (stageStartTime + stages[numStages - 1].durationSec));
  if (complete) {
    digitalWrite(elementPin, LOW);
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("  Cycle Complete");
    lcd.setCursor(0, 1); lcd.print("  Element OFF");
    lcd.setCursor(0, 2); lcd.print("  Total time:");
    lcd.setCursor(0, 3);
    printHMS(elapsed);
    Serial.println("Complete.");
    while (true) {}
  }

  // ── Remaining and elapsed time ─────────────────────────────────────────────
  unsigned long totalTime     = totalProfileTime(currentProfile);
  unsigned long remainingTime = (totalTime > 0 && elapsed < totalTime) ?
                                 (totalTime - elapsed) : 0;

  // ── Element control ────────────────────────────────────────────────────────
  if (temperature < (targetTemp - 8)) {
    digitalWrite(elementPin, HIGH);
  } else if (temperature <= targetTemp) {
    digitalWrite(elementPin, (millis() % 1000 < 300) ? HIGH : LOW);
  } else if (temperature <= (targetTemp + 5)) {
    digitalWrite(elementPin, (millis() % 2000 < 200) ? HIGH : LOW);
  } else {
    digitalWrite(elementPin, LOW);
  }

  // ── Serial ─────────────────────────────────────────────────────────────────
  Serial.print(temperature, 2);
  Serial.print(" ");
  Serial.println(targetTemp, 2);

  // ── LCD ────────────────────────────────────────────────────────────────────
  lcd.setCursor(0, 0);
  lcd.print("Cycle  : ");
  lcd.print(s.name);

  lcd.setCursor(0, 1);
  if (totalTime > 0) {
    lcd.print("Remain : ");
    printHMS(remainingTime);
  } else {
    // Indefinite hold — show elapsed time instead
    lcd.print("Elapsed: ");
    printHMS(elapsed);
  }

  lcd.setCursor(0, 2);
  lcd.print("Target : ");
  lcd.print(targetTemp, 0);
  lcd.print("C       ");

  lcd.setCursor(0, 3);
  lcd.print("Actual : ");
  lcd.print(temperature, 0);
  lcd.print("C       ");

  delay(100);
}
