# ESP32 Oven Controller with Google Sheets Integration

## Overview

This project is a **network-connected oven controller** built on an ESP32.
It performs:

* Temperature control using a thermocouple (MAX6675)
* Multi-stage heating profiles (ramp + soak)
* Remote control via Google Sheets
* Secure data logging with HMAC authentication
* Local control via physical RUN/STOP buttons
* Persistent configuration storage using SPIFFS

---

## System Architecture

### ESP32 (Device)

* Executes heating profile
* Controls SSR (Solid State Relay)
* Reads temperature via MAX6675
* Sends telemetry to Google Sheets
* Receives RUN/STOP commands
* Stores configuration locally in flash (SPIFFS)

### Google Apps Script (Server)

* Acts as API endpoint
* Stores and serves configuration
* Logs temperature data
* Manages device commands
* Provides basic security (API key + HMAC)

Reference:


---

## Features

### Control

* Local RUN / STOP buttons
* Remote RUN / STOP via Google Sheets
* Run lock after profile completion

### Heating Profile

* Up to 8 stages
* Each stage:

  * Target temperature
  * Duration
  * Stage name

### Duty Cycle Control

* Temperature-based duty bands
* Configurable via Google Sheets
* PWM-based SSR control (1-second window)

### Data Logging

* Sends data every 20 seconds:

  * Time elapsed
  * Current temperature
  * Target temperature
  * Stage name
* Logged into per-device sheets

### Security

* Device-specific API keys
* HMAC-SHA256 request signing
* Timestamp validation (anti-replay)

### Offline Support

* Loads last known config from SPIFFS if WiFi unavailable

---

## Hardware

### Required Components

* ESP32-WROOM-32
* MAX6675 thermocouple amplifier
* K-type thermocouple
* Solid State Relay (SSR)
* Heating element
* Push buttons (RUN / STOP)

---

## Pin Configuration

| Function         | GPIO |
| ---------------- | ---- |
| SSR Control      | 4    |
| Indicator LED    | 2    |
| Thermocouple SCK | 18   |
| Thermocouple CS  | 5    |
| Thermocouple SO  | 19   |
| RUN Button       | 16   |
| STOP Button      | 17   |

---

## Configuration Storage

* Stored in SPIFFS as `/config.json`
* Written only when configuration changes
* Flash-safe implementation (write minimised)

### Structure:

```
{
  "profile": [...],
  "dutyCycles": [...]
}
```

---

## Google Sheets Integration

### Sheet Structure

#### Controller Settings

* Columns:

  * A: Temperature
  * B: Duration (seconds)
  * C: Stage Name
  * D–O: Duty cycle bands

#### Master Sheet

* Device status dashboard:

  * Command
  * Current temp
  * Target temp
  * Stage
  * Time remaining
  * Finish time
  * Last seen

#### Device Sheets (e.g. oven_1)

* Each run stored in a new column
* Data format:

```
timeSeconds,currentTemp,targetTemp
```

---

## API Endpoints

### Get Settings

```
?type=settings&device=oven_1&key=API_KEY
```

### Get Command

```
?type=control&device=oven_1&key=API_KEY
```

### Send Data

```
?type=log&device=oven_1&csv=...&temp=...&timeleft=...&stageName=...&ts=...&sig=...
```

### Button Event

```
?type=button&device=oven_1&command=RUN|STOP
```

---

## Control Logic

### Heating Profile Execution

* Linear interpolation between stages
* Tracks:

  * Current stage index
  * Stage name
  * Time remaining

### SSR Control

* Duty cycle based on temperature error
* Uses lookup table:

```
error = targetTemp - currentTemp
→ maps to duty cycle %
```

---

## Flash Memory Strategy

* Configuration saved only when changed
* Prevents unnecessary flash wear
* Safe for long-term operation

---

## Safety Notes

* SSR failure mode should be considered (fail closed possible)
* Recommended:

  * Secondary cutoff (thermal fuse or relay)
  * Independent over-temperature protection

---

## Known Limitations

* Serial output may show encoding issues for special characters (°, Δ)
* Timing depends on WiFi latency for remote control
* No PID control (uses duty band approximation)

---

## Future Improvements

* PID control option
* OTA updates
* Web interface (local AP)
* Data visualization dashboard
* Redundant safety cutoff

---

## Summary

This system provides a **robust, remotely configurable, and flash-safe oven controller** using ESP32 and Google Sheets, with strong emphasis on:

* Reliability
* Simplicity
* Safety
* Maintainability

---
