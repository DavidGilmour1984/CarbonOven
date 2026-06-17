# Kiln Controller Firmware

ESP32-based oven/kiln controller with Google Sheets integration, offline fallback profiles, LCD display, and buzzer feedback.

---

## Hardware

### Pin Assignments

| Function | GPIO |
|---|---|
| LCD SDA (I2C) | 21 |
| LCD SCL (I2C) | 22 |
| MAX6675 CS | 5 |
| MAX6675 SCK | 18 |
| MAX6675 SO | 19 |
| START button | 4 |
| STOP button | 17 |
| SSR output | 32 |
| Buzzer | 26 |

All buttons are wired active-low with `INPUT_PULLUP`. The SSR and buzzer share the same ground reference as the ESP32.

### Required Components

- ESP32 development board
- MAX6675 thermocouple amplifier + K-type thermocouple
- 16×2 I2C LCD (address `0x27`)
- Solid State Relay (SSR)
- Piezo buzzer (passive)
- 2× momentary push buttons (START, STOP)

---

## Libraries Required

Install all via Arduino Library Manager before compiling.

| Library | Version tested |
|---|---|
| LiquidCrystal I2C | 1.1.2 |
| MAX6675 | any |
| ArduinoJson | 7.x |
| ArduinoJson | bundled with ESP32 core |
| WiFi, HTTPClient, WiFiClientSecure, SPIFFS | bundled with ESP32 core |

> **Arduino core version:** This sketch requires **ESP32 Arduino core v3.x**. The older `ledcSetup()` / `ledcAttachPin()` API is not used — it has been replaced with `ledcAttach()`.

> **LiquidCrystal conflict:** If compiling in Arduino Cloud, the `LCDBigNumbers` library may conflict. Install locally with the desktop IDE to avoid this.

---

## Configuration

Open `kiln_controller.ino` and edit the following defines at the top of the file.

### WiFi

```cpp
#define WIFI_SSID      "your_network_name"
#define WIFI_PASSWORD  "your_password"
#define WIFI_TIMEOUT_MS 20000   // seconds to wait before entering offline mode
```

### Device Identity

```cpp
#define DEVICE_ID  "oven_1"   // must match a key in the Google Sheet DEVICE_KEYS map
```

Each physical oven gets a unique `DEVICE_ID`. Currently supported IDs in the Google Apps Script: `oven_1` through `oven_8`.

### Security

```cpp
String API_KEY = "";   // unique per oven — must match DEVICE_KEYS[device] in the script
String SECRET  = "";   // shared HMAC secret — same value on all ovens and in the script
```

Every data upload is signed with HMAC-SHA256 using the `SECRET`. The Google Apps Script rejects any request with a missing, wrong, or stale signature.

---

## Google Sheets Integration

The firmware communicates with a Google Apps Script web app. The script URL is set in:

```cpp
#define SCRIPT_URL "https://script.google.com/macros/s/..."
```

### API Endpoints

| `type=` | Direction | Purpose |
|---|---|---|
| `settings` | ESP32 ← Sheet | Fetch firing profile and duty cycle bands on boot |
| `log` | ESP32 → Sheet | Upload temperature data every 20 seconds |
| `control` | ESP32 ← Sheet | Poll for remote RUN/STOP command every ~10 seconds |
| `button` | ESP32 → Sheet | Notify sheet when a physical button is pressed |

### Controller Settings Sheet

The Google Sheet must have a tab named **Controller Settings** with:

- **Column A:** Target temperature (°C)
- **Column B:** Duration (seconds)
- **Column C:** Stage name
- **Columns D–O:** Duty cycle band pairs (T1, DC1, T2, DC2 … T6, DC6)

Duty cycle bands are sorted descending by temperature delta before being sent to the device.

---

## Startup Sequence

On power-on the device follows this sequence, showing status on the LCD at each step:

1. **Boot splash** — device ID shown
2. **WiFi connect** — progress dots scroll across line 2, times out after 20 seconds
3. **If WiFi OK:**
   - Shows device ID and IP address for 2.5 seconds
   - Syncs time via NTP (required for HMAC timestamp validation)
   - Fetches firing profile from Google Sheet
   - Falls back to cached flash profile if sheet is unreachable
   - Falls back to built-in default profile if flash is also empty
4. **If WiFi fails:**
   - Enters offline mode immediately
   - Attempts to load cached profile from flash
   - Falls back to built-in default profile
5. **Ready** — two-tone beep, idle screen shown

---

## Operation

### Online Mode

| Action | Result |
|---|---|
| Press START | Begins firing profile, notifies Google Sheet |
| Press STOP | Stops immediately, SSR off, notifies Google Sheet |
| Remote RUN (from sheet) | Starts profile if not already running |
| Remote STOP (from sheet) | Stops profile |

The device polls for remote commands every ~10 seconds and uploads temperature data every 20 seconds.

### Offline Mode

Offline mode is entered at boot if WiFi does not connect within 20 seconds. In offline mode:

| Action | Result |
|---|---|
| Press STOP (when idle) | Cycles between built-in profiles |
| Press START | Runs the currently selected profile |
| Press STOP (when running) | Stops the run |

No data is uploaded and no remote commands are received in offline mode.

---

## Built-in Default Profiles

Two profiles are stored in flash and available without any network connection.

### Profile 0 — Test Cycle (15 minutes)

| Stage | Target | Duration |
|---|---|---|
| Ramp Up | 40°C | 5 min |
| Soak | 40°C | 10 min |
| Cool Down | 5°C | 5 min |

Used for hardware verification and thermocouple calibration checks.

### Profile 1 — Carbon Fibre Cure (approx. 6.5 hours)

Standard out-of-autoclave (OOA) epoxy prepreg cure cycle.

| Stage | Target | Duration |
|---|---|---|
| Ramp to Flow | 70°C | 50 min (~1°C/min) |
| Flow Soak | 70°C | 60 min |
| Ramp to Cure | 125°C | 55 min (~1°C/min) |
| Cure Soak | 125°C | 90 min |
| Cool Down | 40°C | 165 min (~0.5°C/min) |

The slow cool-down rate minimises distortion risk when removing parts from the mould.

> The Google Sheet profile overrides these defaults whenever WiFi is available. The defaults are only used when the sheet and flash cache are both unavailable, or when manually selected in offline mode.

---

## LCD Display

### Idle screen

```
oven_1  W  24.3C
IDLE
```

The status character on line 1 is `W` (WiFi connected), `X` (offline mode), or `?` (WiFi lost mid-session).

### Running screen

```
C:68.2  T:70.0 *
Flow Soak  42:17
```

- `C:` — current temperature
- `T:` — target temperature at this point in the profile
- `*` — SSR is currently ON (`-` when OFF)
- Stage name truncated to 11 characters
- Remaining time in `MM:SS`

### Error messages

Errors appear as `ERR:` on line 1 with a description on line 2 and clear automatically after 1.5–2 seconds. Possible errors:

| Error | Cause |
|---|---|
| `ERR:SPIFFS` | Flash filesystem failed to mount |
| `ERR:FLASH` | Config file could not be read or written |
| `ERR:WIFI` | No WiFi connection at boot |
| `ERR:SHEET` | HTTP error or bad JSON from Google Sheet |
| `ERR:TIME` | NTP sync failed |
| `ERR:NO PROFILE` | No profile from sheet, flash, or defaults |

---

## SSR Control

The SSR is controlled with a 1-second PWM window. The duty cycle is determined by a lookup table of temperature-error bands fetched from the Google Sheet (or the built-in defaults when offline).

Default duty cycle bands:

| Error (target − current) | Duty cycle |
|---|---|
| > 5°C below target | 100% |
| 3–5°C below | 80% |
| 1–3°C below | 50% |
| 0–1°C below | 20% |
| 0–1°C above | 10% |
| > 1°C above | 0% (off) |

The SSR is also forced off immediately if the current temperature exceeds the target by more than 1°C.

---

## Buzzer Feedback

| Event | Pattern |
|---|---|
| Boot ready | Two rising tones |
| RUN started | Single mid tone |
| STOP pressed | Single low tone |
| Profile complete | Three rising tones |
| Offline profile switch | Two-tone confirmation |

All beeps are non-blocking — they do not pause the control loop.

---

## Deploying to Multiple Ovens

1. Copy the sketch folder for each oven
2. Change `DEVICE_ID` to match (`oven_1`, `oven_2`, etc.)
3. Set a unique `API_KEY` matching the value in `DEVICE_KEYS` in the Google Apps Script
4. Keep `SECRET` identical across all ovens and the script
5. Flash and deploy

The `WIFI_SSID` / `WIFI_PASSWORD` and `SCRIPT_URL` are the same for all ovens.
