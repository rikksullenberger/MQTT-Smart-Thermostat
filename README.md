# Armenda Thermostat (Waveshare ESP32‑S3‑Relay‑6CH)

A Wi‑Fi + MQTT smart thermostat for Home Assistant with captive‑portal setup, full MQTT Discovery, a tidy web UI, and a single WS2812 status LED that tells you what the HVAC is up to at a glance.

> **Board:** Waveshare ESP32‑S3‑Relay‑6CH
> **Device ID:** `main_thermostat`
> **Default MQTT base topic:** `thermo/main_thermostat`

---

## ✨ Features

* **Zero‑app onboarding** with WiFiManager captive portal (SSID: `ArmendaThermostat-Setup`).
* **MQTT Discovery** for Home Assistant (climate + temperature + humidity entities).
* **Web interface** to view status and tweak parameters:

  * Set HVAC mode (Off / Heat / Cool / Auto / Fan Only)
  * Set target temperature (°F)
  * Adjust protections: min on/off, deadband, stage‑2 delta + delay, blower w/heat
  * Manually update temp/humidity (for testing)
* **Compressor protections** (min ON/OFF), **dual‑stage heat** with programmable delta and delay.
* **WS2812 status LED** on GPIO38:

  * Cooling → **Blue**
  * Heat Stage 1 → **Orange**
  * Heat Stage 2 → **Red**
  * Fan → **Green**
  * Idle → **White**
  * Off → **Off**
  * Compressor lockout → **Purple blink**
  * Portal/connect mode → **Cyan pulse**
* **mDNS**: reachable at `http://armenda-thermostat.local/` (when supported).

---

## 🧱 Hardware & Wiring

**Target board:** Waveshare ESP32‑S3‑Relay‑6CH

Relay channel mapping (set in code):

| HVAC Signal          | Relay Channel |  ESP32 Pin |
| -------------------- | ------------: | ---------: |
| G (Fan)              |           CH1 |      GPIO1 |
| W1 (Heat 1)          |           CH2 |      GPIO2 |
| W2 (Heat 2)          |           CH3 |     GPIO41 |
| Y1 (Cool/Compressor) |           CH4 |     GPIO42 |
| Spare                |           CH5 |     GPIO45 |
| Spare                |           CH6 |     GPIO46 |
| WS2812 LED           |             — | **GPIO38** |

> ⚠️ **High‑voltage caution:** This project switches HVAC control circuits. Double‑check wiring, power off your air handler/thermostat circuit at the breaker, and proceed only if you're comfortable with low‑voltage HVAC wiring.

---

## 🏠 Home Assistant Integration

This device auto‑registers with Home Assistant via MQTT Discovery.

**Discovery topics:**

* Climate: `homeassistant/climate/armenda/main_thermostat/config`
* Temperature sensor: `homeassistant/sensor/armenda/main_thermostat_temp/config`
* Humidity sensor: `homeassistant/sensor/armenda/main_thermostat_humidity/config`

**Entities created:**

* `climate.armenda_thermostat` (name shown as **Armenda Thermostat**)
* `sensor.armenda_thermostat_temperature` (°F)
* `sensor.armenda_thermostat_humidity` (%)

**Supported modes (mapped to HA):** `off`, `heat`, `cool`, `auto` (→ `heat_cool`), `fan_only`

**Precision:** `0.1` (°F)
**Units:** Fahrenheit only (as coded)

---

## 🌐 Web UI

Once connected to Wi‑Fi, open the device IP (or `http://armenda-thermostat.local/`).

**Routes**

* `GET /` — Dashboard (current status, mode/temperature controls)
* `GET /config` — HVAC protection parameters
* `GET /portal` — Start WiFiManager portal (blocking until exit)
* `POST /setmode` — Form post with `mode`
* `POST /settemp` — Form post with `temp`
* `POST /setsensors` — Form post with `temp_f`, `humidity`
* `POST /saveconfig` — Form post with protection parameters

---

## 📡 MQTT Topics & Payloads

**Base:** `thermo/main_thermostat`

| Purpose                | Topic                                                               |
| ---------------------- | ------------------------------------------------------------------- |
| Availability (LWT)     | `thermo/main_thermostat/availability` (`online`/`offline` retained) |
| State/attributes       | `thermo/main_thermostat/state` (JSON, retained)                     |
| Commands               | `thermo/main_thermostat/cmd` (JSON)                                 |
| Ambient sensor updates | `thermo/main_thermostat/ambient` (JSON)                             |

### State payload (published every \~5s and on change)

```json
{
  "mode": "heat_cool",
  "action": "idle",
  "current_temp": 72.0,
  "target_temp": 72.0,
  "humidity": 45.0,
  "units": "F",
  "min_on_s": 300,
  "min_off_s": 300,
  "deadband_f": 0.8,
  "stage2_delta_f": 2.0,
  "stage2_delay_s": 600,
  "fan_with_heat": false
}
```

### Command payloads (`/cmd`)

Set mode and target temperature:

```json
{ "mode": "cool", "target_temp_f": 71.5 }
```

Supported `mode` values: `off`, `heat`, `cool`, `heat_cool`, `fan_only`

Live‑tune protections:

```json
{
  "min_on_s": 420,
  "min_off_s": 420,
  "deadband_f": 1.0,
  "stage2_delta_f": 2.5,
  "stage2_delay_s": 900,
  "fan_with_heat": true
}
```

Open Wi‑Fi portal from HA (blocks until portal exit):

```json
{ "portal": true }
```

Factory Wi‑Fi reset (erases saved Wi‑Fi & MQTT prefs, then reboots):

```json
{ "wifi_reset": true }
```

### Ambient sensor payloads (`/ambient`)

Use this if you’re feeding temperature/humidity from an external sensor.

```json
{ "temp_f": 73.2, "humidity": 41.5 }
```

---

## 🧠 Control Logic Overview

* **Deadband:** ±`deadband_f/2` around `target_temp_f`.
* **Cooling:**

  * Respects **min OFF** before starting compressor.
  * Runs G (fan) with Y1; LED turns **Blue**.
  * Respects **min ON** before permitting stop.
* **Heating:**

  * W1 triggers when below `low = target - deadband/2`.
  * W2 triggers if `(target - current) ≥ stage2_delta_f` **and** heat call has lasted ≥ `stage2_delay_s`.
  * Optional `fan_with_heat` turns on G during heat.
  * LED **Orange** (W1) / **Red** (W2).
* **Fan Only:** G on; LED **Green**.
* **Compressor lockout:** LED **Purple blink** when min OFF prevents a start.

---

## ⚙️ Configuration Storage

Preferences namespace `thermo`:

* `ha_ip` (string)
* `mqtt_host` (string; blank → use `ha_ip`)
* `mqtt_port` (uint16; default 1883)
* `mqtt_user` (string)
* `mqtt_pass` (string)

On first boot (no `ha_ip` stored) the device automatically opens the portal.

---

## 🔧 Build & Flash

You can use **Arduino IDE** or **PlatformIO**.

### Dependencies

* [WiFiManager](https://github.com/tzapu/WiFiManager)
* [PubSubClient](https://pubsubclient.knolleary.net/)
* [ArduinoJson](https://arduinojson.org/) (6.x)
* [Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel)

### Arduino IDE (ESP32)

1. Install **ESP32** board support (ESP32‑S3).
2. Select **ESP32S3 Dev Module** (or the closest Waveshare S3 variant).
3. Install the libraries above (Library Manager).
4. Compile & upload the provided sketch.

### PlatformIO example

`platformio.ini` example:

```ini
[env:esp32s3]
platform = espressif32
board = esp32s3dev
framework = arduino
build_flags =
  -DCORE_DEBUG_LEVEL=0
lib_deps =
  tzapu/WiFiManager @ ^2.0.17
  knolleary/PubSubClient @ ^2.8
  bblanchon/ArduinoJson @ ^6.21.3
  adafruit/Adafruit NeoPixel @ ^1.12.3
monitor_speed = 115200
```

> Tip: If your Waveshare variant needs different upload settings (USB CDC, PSRAM, etc.), adjust `board_build` flags accordingly.

---

## 🚀 First‑Time Setup

1. Power the device. The LED **cyan‑pulses** and the AP `ArmendaThermostat-Setup` appears.
2. Connect and open the portal page:

   * **Home Assistant IP** (e.g., `192.168.30.10`)
   * **MQTT Host** (optional; leave blank to use the HA IP)
   * **MQTT Port/User/Pass** (optional; port defaults to 1883)
3. Save and exit. Device reboots, connects, and publishes MQTT Discovery.
4. In Home Assistant, the **Armenda Thermostat** climate + sensors should appear.

---

## 🧪 cURL Examples

Set cooling to 71.5 °F via MQTT (mosquitto):

```bash
mosquitto_pub -h <broker> -t thermo/main_thermostat/cmd -m '{"mode":"cool","target_temp_f":71.5}'
```

Feed external ambient values:

```bash
mosquitto_pub -h <broker> -t thermo/main_thermostat/ambient -m '{"temp_f":73.2,"humidity":41.5}'
```

Open captive portal remotely (device will block until portal closes):

```bash
mosquitto_pub -h <broker> -t thermo/main_thermostat/cmd -m '{"portal":true}'
```

---

## 🔍 Troubleshooting

* **HA entity not showing:** verify broker, see if `homeassistant/status` is `online`. On that event the device republishes discovery and state.
* **LED pulses cyan forever:** Wi‑Fi not connected → trigger the captive portal (power‑cycle or `portal:true`).
* **Compressor won’t start right away:** min OFF timer active → LED blinks purple. Wait until `min_off_s` expires.
* **No updates in HA:** ensure `thermo/main_thermostat/availability` is `online` and you can see retained `.../state` in your broker.

---

## 🗺️ Roadmap

* Celsius support & automatic unit handling
* Optional sensor auto‑ingest from ESPHome/HTTP
* Stage‑2 cooling (Y2) support (requires hardware)
* OTA updates page in web UI

---

## 📜 License

MIT (or your preferred OSI license — update before publishing.)

---

## 🙏 Credits

* Built with ❤️ on **ESP32‑S3** & **Waveshare Relay 6‑CH**.
* Libraries: WiFiManager, PubSubClient, ArduinoJson, Adafruit NeoPixel.

---

## 📎 Appendix: Quick Reference

* **mDNS:** `armenda-thermostat.local`
* **LED Map:** Cooling=Blue | Heat1=Orange | Heat2=Red | Fan=Green | Idle=White | Off=Off | Lockout=Purple blink | Portal=Cyan pulse
* **Web:** `/`, `/config`, `/portal`, `POST /setmode`, `/settemp`, `/setsensors`, `/saveconfig`
* **MQTT:** base `thermo/main_thermostat` with `.../state`, `.../cmd`, `.../ambient`, `.../availability`
