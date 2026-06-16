#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <max6675.h>

// =====================
// PINS
// =====================
#define LCD_SDA_PIN 21
#define LCD_SCL_PIN 22

#define MAX6675_CS   5
#define MAX6675_SCK 18
#define MAX6675_SO  19

#define START_BUTTON 4
#define STOP_BUTTON 17

#define LED1_PIN 27
#define LED2_PIN 33
#define SSR_PIN 32

#define BUZZER_PIN 26

// =====================
// LCD
// =====================
LiquidCrystal_I2C lcd(0x27, 16, 2);

// =====================
// MAX6675
// =====================
MAX6675 thermocouple(MAX6675_SCK, MAX6675_CS, MAX6675_SO);

// =====================
// TIMERS
// =====================
unsigned long lastLCD = 0;
unsigned long lastDiag = 0;

// =====================
// BUZZER STATE (ESP32 3.x FIX)
// =====================
bool buzzerActive = false;
unsigned long buzzerOffTime = 0;

// =====================
// BUZZER FUNCTIONS
// =====================
void buzzerBegin()
{
  ledcAttach(BUZZER_PIN, 2000, 8);
  ledcWriteTone(BUZZER_PIN, 0);
}

void beep(int freq, int durationMs)
{
  ledcWriteTone(BUZZER_PIN, freq);
  buzzerActive = true;
  buzzerOffTime = millis() + durationMs;
}

// =====================
// SETUP
// =====================
void setup()
{
  Serial.begin(9600);

  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);

  pinMode(START_BUTTON, INPUT_PULLUP);
  pinMode(STOP_BUTTON, INPUT_PULLUP);

  pinMode(LED1_PIN, OUTPUT);
  pinMode(LED2_PIN, OUTPUT);
  pinMode(SSR_PIN, OUTPUT);

  lcd.init();
  lcd.backlight();

  lcd.setCursor(0, 0);
  lcd.print("SYSTEM START");
  lcd.setCursor(0, 1);
  lcd.print("INIT...");

  buzzerBegin();
}

// =====================
// LOOP
// =====================
void loop()
{
  unsigned long now = millis();

  // =====================
  // STOP BUZZER NON-BLOCKING
  // =====================
  if (buzzerActive && now > buzzerOffTime)
  {
    ledcWriteTone(BUZZER_PIN, 0);
    buzzerActive = false;
  }

  // =====================
  // TEMP READ (stable timing)
  // =====================
  static unsigned long lastTemp = 0;
  static float tempC = 0;

  if (now - lastTemp >= 250)
  {
    lastTemp = now;
    tempC = thermocouple.readCelsius();
  }

  // =====================
  // HARDWARE TEST (1Hz)
  // =====================
  if (now - lastDiag >= 1000)
  {
    lastDiag = now;

    static bool state = false;
    state = !state;

    digitalWrite(LED1_PIN, state);
    digitalWrite(LED2_PIN, state);
    digitalWrite(SSR_PIN, state);

    beep(2000, 30);
  }

  bool startPressed = (digitalRead(START_BUTTON) == LOW);
  bool stopPressed  = (digitalRead(STOP_BUTTON) == LOW);

  // =====================
  // LCD LIVE UPDATE
  // =====================
  if (now - lastLCD >= 200)
  {
    lastLCD = now;

    lcd.setCursor(0, 0);
    lcd.print("T:");
    lcd.print(tempC, 1);
    lcd.print("C      ");

    lcd.setCursor(0, 1);

    if (startPressed)
      lcd.print("START ACTIVE   ");
    else if (stopPressed)
      lcd.print("STOP ACTIVE    ");
    else
      lcd.print("SYSTEM OK      ");
  }

  // SERIAL DEBUG
  Serial.println(tempC);
}
