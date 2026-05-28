#include "max6675.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

int thermoDO = 11;
int thermoCS = 10;
int thermoCLK = 9;
int startPin = 12;
int elementPin = 13;

const float targetTemperature = 120; //Target temperature for end of First ramp
const float targetTemperature2 = 195; //Target temperature for end of Second ramp
const float targetTemperature3 = 0; //Target temperature for end of Third ramp
float startTemperature = 0;
const int rampTime = 90; //Time of First ramp
const int soakTime = 90; //Time of First soak
const int rampTime2 = 20; //Time of Second ramp
const int soakTime2 = 100;  //Time of Second soak
const int rampTime3 = 5;  //Time of Third ramp
float rateOfChange = 0;
float rateOfChange2 = 0;
float rateOfChange3 = 0;

MAX6675 thermocouple(thermoCLK, thermoCS, thermoDO);
unsigned long startTime;
bool rampTimePassed = false;
bool rampTimePassed2 = false;
bool rampTimePassed3 = false;  // New flag for the third cycle
bool soakTimePassed = false;
float targetTemp = 0;
String currentCycle = "";

LiquidCrystal_I2C lcd(0x27, 24, 4); // initialize the LCD

void setup() {
pinMode(startPin, INPUT);
pinMode(elementPin, OUTPUT);
Serial.begin(9600);
Serial.println("Thermocouple test pass");

// wait for MAX chip to stabilize
delay(500);

startTemperature = thermocouple.readCelsius();
rateOfChange = (targetTemperature - startTemperature) / rampTime;
rateOfChange2 = (targetTemperature2 - targetTemperature) / rampTime2;
rateOfChange3 = (targetTemperature3 - targetTemperature2) / rampTime3;  // New rate of change for the third cycle

lcd.init(); // start the LCD
lcd.backlight();
lcd.setCursor(0, 1); // set cursor to column 0, row 1
lcd.print("  RSRSR Contoller");
lcd.setCursor(0, 2); // set cursor to column 0, row 2
lcd.print("      SN42BI58");


// Wait for the start pin to be pulled high
while (digitalRead(startPin) != HIGH) {
delay(10);
}
startTime = millis();
}

void loop() {
float temperature = thermocouple.readCelsius();
if (temperature == 0) { // check if the returned temperature is 0
Serial.println("Error reading temperature. Check thermocouple connection.");
return; // exit the loop to prevent further execution
}


// Calculate the target temperature based on time elapsed
if ((millis() - startTime) / 1000 < rampTime)
targetTemp = startTemperature + rateOfChange * (millis() - startTime) / 1000;
else if ((millis() - startTime) / 1000 >= rampTime && !rampTimePassed) {
targetTemp = targetTemperature;
rampTimePassed = true;
} else if (rampTimePassed && (millis() - startTime) / 1000 < (rampTime + soakTime))
targetTemp = targetTemperature;
else if (rampTimePassed && !soakTimePassed && (millis() - startTime) / 1000 >= (rampTime + soakTime)) {
soakTimePassed = true;
startTemperature = targetTemperature; // start temp for 2nd ramp
rateOfChange = (targetTemperature2 - startTemperature) / rampTime2;
} else if (soakTimePassed && (millis() - startTime) / 1000 < (rampTime + soakTime + rampTime2))
targetTemp = startTemperature + rateOfChange * ((millis() - startTime) / 1000 - (rampTime + soakTime));
else if (soakTimePassed && !rampTimePassed2 && (millis() - startTime) / 1000 >= (rampTime + soakTime + rampTime2)) {
targetTemp = targetTemperature2;
rampTimePassed2 = true;
} else if (rampTimePassed2 && (millis() - startTime) / 1000 < (rampTime + soakTime + rampTime2 + soakTime2))
targetTemp = targetTemperature2;
else if (rampTimePassed2 && !rampTimePassed3 && (millis() - startTime) / 1000 >= (rampTime + soakTime + rampTime2 + soakTime2)) {
soakTimePassed = true;
rampTimePassed3 = true;
startTemperature = targetTemperature2; // start temp for 3rd ramp
rateOfChange = (targetTemperature3 - startTemperature) / rampTime3;
} else if (rampTimePassed3 && (millis() - startTime) / 1000 < (rampTime + soakTime + rampTime2 + soakTime2 + rampTime3))
targetTemp = startTemperature + rateOfChange * ((millis() - startTime) / 1000 - (rampTime + soakTime + rampTime2 + soakTime2));
else if (rampTimePassed3 && !soakTimePassed && (millis() - startTime) / 1000 >= (rampTime + soakTime + rampTime2 + soakTime2 + rampTime3)) {
targetTemp = targetTemperature3;
soakTimePassed = true;
}


// Update current cycle based on the elapsed time
if ((millis() - startTime) / 1000 < rampTime)
currentCycle = "1st Ramp";
else if ((millis() - startTime) / 1000 < (rampTime + soakTime))
currentCycle = "1st Soak ";
else if ((millis() - startTime) / 1000 < (rampTime + soakTime + rampTime2))
currentCycle = "2nd Ramp";
else if ((millis() - startTime) / 1000 < (rampTime + soakTime + rampTime2 + soakTime2))
currentCycle = "2nd Soak";
else if ((millis() - startTime) / 1000 < (rampTime + soakTime + rampTime2 + soakTime2 + rampTime3))
currentCycle = "3rd Ramp";
else
currentCycle = "Complete";

// Calculate the remaining time
unsigned long elapsedTime = (millis() - startTime) / 1000;
unsigned long remainingTime = (rampTime + soakTime + rampTime2 + soakTime2 + rampTime3) - elapsedTime;
int remainingMinutes = remainingTime / 60;
int remainingSeconds = remainingTime % 60;

// Add condition for soak time passed
if (rampTimePassed2 && (millis() - startTime) / 1000 >= (rampTime + soakTime + rampTime2 + soakTime2 + rampTime3)) {
lcd.clear(); // clear the previous message
lcd.setCursor(0, 1);
lcd.print("  Cycles Complete"); // print roast complete message
digitalWrite(elementPin, LOW); // Turn off element

while (true) {}; // infinite loop
}

// This is the rough "PID" hack section

if (temperature < (targetTemp - 8)) { // The negative numebr at the end is the temperature difference below the target temperature that the emement will stay on.
digitalWrite(elementPin, HIGH); // Once it goes above this it will start pulsing

} else if (temperature <= targetTemp) { // if it is within the difference between the number from above and the target temp the pulsing will happen
if (millis() % 1000 < 300) {  // This controls the pulsing. The bigger the left number and smaller the right number the less the pulsing.
digitalWrite(elementPin, HIGH);
} else {
digitalWrite(elementPin, LOW);
}

} else if (temperature <= (targetTemp + 5)) { // The + number is the end is for the temperature that this condition will work up to
if (millis() % 2000 < 200) { // This controls the pulsing. The bigger the left number and smaller the right number the less the pulsing.
digitalWrite(elementPin, HIGH);
} else {
digitalWrite(elementPin, LOW);
}
} else {
digitalWrite(elementPin, LOW);  // If the temperature is above the temperature difference above and the target temperature the element will remain off
}
{
Serial.print(temperature, 2);
Serial.print(" ");
Serial.println(targetTemp, 2);

}

lcd.setCursor(0, 0);
lcd.print("  Cycle   : ");
lcd.print(currentCycle);
lcd.setCursor(0, 1);
lcd.print("Time Left : ");
lcd.print(remainingMinutes);
lcd.print("m ");
lcd.print(remainingSeconds);
lcd.print("s  ");
lcd.setCursor(0, 2);
lcd.print(" Target      Actual");
lcd.setCursor(0, 3);
lcd.print("  ");
lcd.print(targetTemp, 0);
lcd.print("C    :   ");
lcd.print(temperature, 0);
lcd.print("C");

delay(100);
}
