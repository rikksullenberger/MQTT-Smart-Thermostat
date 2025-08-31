// ===== Armenda Thermostat (Waveshare ESP32-S3-Relay-6CH) =====
// Captive portal via WiFiManager: choose SSID, set Home Assistant IP,
// optionally override MQTT host/port/user/pass. If MQTT Host is blank,
// we use the HA IP and port 1883 by default.
// MQTT Discovery auto-creates the climate entity in Home Assistant.
// Web interface for configuration and control.
//
// Relays (per your wiring):
//   G  -> CH1 (GPIO1)
//   W1 -> CH2 (GPIO2)
//   W2 -> CH3 (GPIO41)
//   Y1 -> CH4 (GPIO42)
// WS2812 status LED on GPIO38.
//
// LED colors:
//   Cooling=Blue | Heat1=Orange | Heat2=Red | Fan=Green | Idle=White
//   Off=Off | Compressor lockout=Purple blink | Portal mode=Cyan pulse

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <WiFiManager.h>     // tzapu/WiFiManager
#include <Preferences.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// --------------------- Identity / MQTT topics ---------------------
const char* DEV_ID     = "main_thermostat";
const char* DEV_NAME   = "Armenda Thermostat";
const char* TOPIC_BASE = "thermo/main_thermostat";

// Discovery topic: homeassistant/<component>/<node>/<object>/config
String t_disc    = String("homeassistant/climate/armenda/") + DEV_ID + "/config";
String t_avail   = String(TOPIC_BASE) + "/availability";
String t_state   = String(TOPIC_BASE) + "/state";
String t_cmd     = String(TOPIC_BASE) + "/cmd";
String t_ambient = String(TOPIC_BASE) + "/ambient";

// --------------------- Pins (Waveshare board) ---------------------
constexpr int PIN_G    = 1;   // fan
constexpr int PIN_W1   = 2;   // heat stage 1
constexpr int PIN_W2   = 41;  // heat stage 2
constexpr int PIN_Y1   = 42;  // cool (compressor)
constexpr int PIN_R5   = 45;  // spare
constexpr int PIN_R6   = 46;  // spare
constexpr int PIN_RGB  = 38;  // WS2812

// --------------------- LED (single WS2812) ---------------------
Adafruit_NeoPixel led(1, PIN_RGB, NEO_GRB + NEO_KHZ800);
uint8_t LED_BRIGHT_IDLE  = 8;
uint8_t LED_BRIGHT_RUN   = 22;
uint8_t LED_BRIGHT_ALERT = 30;

inline void ledShow(uint8_t r, uint8_t g, uint8_t b, uint8_t br) {
  led.setBrightness(br);
  led.setPixelColor(0, led.Color(r, g, b));
  led.show();
}
inline void setLedOff()        { ledShow(0,   0,   0,   0); }
inline void setLedIdle()       { ledShow(255, 255, 255, LED_BRIGHT_IDLE); }
inline void setLedCooling()    { ledShow(0,   80,  255, LED_BRIGHT_RUN); }
inline void setLedHeat1()      { ledShow(255, 80,  0,   LED_BRIGHT_RUN); }
inline void setLedHeat2()      { ledShow(255, 0,   0,   LED_BRIGHT_RUN); }
inline void setLedFan()        { ledShow(0,   255, 80,  LED_BRIGHT_RUN); }
inline void blinkLockout()     { ledShow(180, 0, 180, LED_BRIGHT_ALERT); delay(120); setLedIdle(); }
inline void pulsePortal()      { ledShow(0, 200, 200, LED_BRIGHT_RUN); delay(120); setLedOff(); delay(120); } // cyan

// --------------------- Runtime state ---------------------
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Preferences prefs;
WebServer server(80);

String  cfg_ha_ip;          // set in portal
String  cfg_mqtt_host;      // optional override; if empty -> use ha_ip
uint16_t cfg_mqtt_port = 1883;
String  cfg_mqtt_user;      // optional
String  cfg_mqtt_pass;      // optional

float currentTempF = 72.0;
float targetTempF  = 72.0;
float humidity     = 45.0;

enum Mode { M_OFF, M_HEAT, M_COOL, M_HEATCOOL, M_FANONLY };
Mode   hvacMode    = M_OFF;
String hvacModeStr = "off";
String hvacAction  = "idle";

bool     y1_on = false;
uint32_t y1_last_change = 0;  // seconds since boot
bool     w1_on = false;
bool     w2_on = false;
uint32_t w_call_start = 0;    // when heat call started (for stage2 delay)

// Protections / behavior (tweakable via /cmd JSON)
uint32_t MIN_ON_SEC        = 300;   // compressor min ON  (5 min)
uint32_t MIN_OFF_SEC       = 300;   // compressor min OFF (5 min)
float    DEADBAND_F        = 0.8;   // +/- around target
float    STAGE2_DELTA_F    = 2.0;   // stage 2 threshold
uint32_t STAGE2_DELAY_SEC  = 600;   // wait before W2
bool     FAN_WITH_HEAT     = false; // many furnaces manage blower

uint32_t now_s() { return millis() / 1000; }
void setRelay(int pin, bool on) { digitalWrite(pin, on ? HIGH : LOW); }
void allOff() { setRelay(PIN_G,false); setRelay(PIN_W1,false); setRelay(PIN_W2,false); setRelay(PIN_Y1,false); }

// --------------------- Prototypes ---------------------
void publishDiscovery();
void publishState();
void applyOutputs();
void handleCmd(const JsonVariant& j);
void handleAmbient(const JsonVariant& j);
void onMqtt(char* topic, byte* payload, unsigned int len);
void ensureMqtt();
bool runConfigPortal(bool eraseWifi);
void startWebServer();

// --------------------- MQTT helpers ---------------------
void publishAvailability(const char* s) { mqtt.publish(t_avail.c_str(), s, true); }

void publishState() {
  StaticJsonDocument<512> d;
  d["mode"]            = hvacModeStr;
  d["action"]          = hvacAction;
  d["current_temp"]    = currentTempF;
  d["target_temp"]     = targetTempF;
  d["humidity"]        = humidity;
  d["units"]           = "F";
  d["min_on_s"]        = MIN_ON_SEC;
  d["min_off_s"]       = MIN_OFF_SEC;
  d["deadband_f"]      = DEADBAND_F;
  d["stage2_delta_f"]  = STAGE2_DELTA_F;
  d["stage2_delay_s"]  = STAGE2_DELAY_SEC;
  d["fan_with_heat"]   = FAN_WITH_HEAT;
  char buf[512];
  size_t n = serializeJson(d, buf, sizeof(buf));
  mqtt.publish(t_state.c_str(), (const uint8_t*)buf, n, true);
}

// Enhanced discovery - includes climate + temperature/humidity sensors
void publishDiscovery() {
  // Climate entity (your existing functionality)
  StaticJsonDocument<1200> d;

  d["name"]      = DEV_NAME;
  d["uniq_id"]   = DEV_ID;
  d["availability_topic"]    = t_avail;
  d["json_attributes_topic"] = t_state;

  d["current_temperature_topic"]    = t_state;
  d["current_temperature_template"] = "{{ value_json.current_temp }}";

  d["temperature_state_topic"]      = t_state;
  d["temperature_state_template"]   = "{{ value_json.target_temp }}";
  d["temperature_command_topic"]    = t_cmd;
  d["temperature_command_template"] = "{\"target_temp_f\": {{ value }} }";

  d["mode_state_topic"]             = t_state;
  d["mode_state_template"]          = "{{ 'auto' if value_json.mode == 'heat_cool' else value_json.mode }}";
  d["mode_command_topic"]           = t_cmd;
  d["mode_command_template"]        = "{\"mode\":\"{{ 'heat_cool' if value == 'auto' else value }}\"}";

  JsonArray modes = d.createNestedArray("modes");
  modes.add("off"); modes.add("heat"); modes.add("cool"); modes.add("auto"); modes.add("fan_only");

  d["temperature_unit"] = "F";
  d["precision"]        = 0.1;

  JsonObject dev = d.createNestedObject("device");
  dev["name"]         = DEV_NAME;
  dev["manufacturer"] = "Waveshare";
  dev["model"]        = "ESP32-S3-Relay-6CH";
  JsonArray ids = dev.createNestedArray("identifiers");
  ids.add(DEV_ID);

  char buf[1200];
  size_t n = serializeJson(d, buf, sizeof(buf));
  mqtt.publish(t_disc.c_str(), (const uint8_t*)buf, n, true);

  // Temperature sensor
  StaticJsonDocument<600> temp_d;
  temp_d["name"] = String(DEV_NAME) + " Temperature";
  temp_d["uniq_id"] = String(DEV_ID) + "_temp";
  temp_d["obj_id"] = String(DEV_ID) + "_temp";
  temp_d["availability_topic"] = t_avail;
  temp_d["state_topic"] = t_state;
  temp_d["value_template"] = "{{ value_json.current_temp }}";
  temp_d["unit_of_measurement"] = "°F";
  temp_d["device_class"] = "temperature";
  temp_d["state_class"] = "measurement";
  
  JsonObject temp_dev = temp_d.createNestedObject("device");
  temp_dev["name"] = DEV_NAME;
  temp_dev["manufacturer"] = "Waveshare";
  temp_dev["model"] = "ESP32-S3-Relay-6CH";
  JsonArray temp_ids = temp_dev.createNestedArray("identifiers");
  temp_ids.add(DEV_ID);

  String t_disc_temp = String("homeassistant/sensor/armenda/") + DEV_ID + "_temp/config";
  char temp_buf[600];
  size_t temp_n = serializeJson(temp_d, temp_buf, sizeof(temp_buf));
  mqtt.publish(t_disc_temp.c_str(), (const uint8_t*)temp_buf, temp_n, true);

  // Humidity sensor
  StaticJsonDocument<600> hum_d;
  hum_d["name"] = String(DEV_NAME) + " Humidity";
  hum_d["uniq_id"] = String(DEV_ID) + "_humidity";
  hum_d["obj_id"] = String(DEV_ID) + "_humidity";
  hum_d["availability_topic"] = t_avail;
  hum_d["state_topic"] = t_state;
  hum_d["value_template"] = "{{ value_json.humidity }}";
  hum_d["unit_of_measurement"] = "%";
  hum_d["device_class"] = "humidity";
  hum_d["state_class"] = "measurement";
  
  JsonObject hum_dev = hum_d.createNestedObject("device");
  hum_dev["name"] = DEV_NAME;
  hum_dev["manufacturer"] = "Waveshare";
  hum_dev["model"] = "ESP32-S3-Relay-6CH";
  JsonArray hum_ids = hum_dev.createNestedArray("identifiers");
  hum_ids.add(DEV_ID);

  String t_disc_hum = String("homeassistant/sensor/armenda/") + DEV_ID + "_humidity/config";
  char hum_buf[600];
  size_t hum_n = serializeJson(hum_d, hum_buf, sizeof(hum_buf));
  mqtt.publish(t_disc_hum.c_str(), (const uint8_t*)hum_buf, hum_n, true);
}

// --------------------- Control logic ---------------------
void updateLed(bool wantY1, bool wantW1, bool wantW2, bool wantG, bool compressorBlocked) {
  if (hvacMode == M_OFF) { setLedOff(); return; }
  if (compressorBlocked) { blinkLockout(); return; }
  if (hvacAction == "cooling" || wantY1)  { setLedCooling(); return; }
  if (hvacAction == "heating" || wantW1 || wantW2) {
    if (wantW2) setLedHeat2(); else setLedHeat1(); return;
  }
  if (hvacAction == "fan" || (hvacMode == M_FANONLY && wantG)) { setLedFan(); return; }
  setLedIdle();
}

void applyOutputs() {
  const float low  = targetTempF - DEADBAND_F / 2.0f;
  const float high = targetTempF + DEADBAND_F / 2.0f;

  bool want_G=false, want_W1=false, want_W2=false, want_Y1=false;
  bool compressorBlocked = false;
  hvacAction = "idle";

  // COOL demand
  if ((hvacMode == M_COOL || hvacMode == M_HEATCOOL) && currentTempF > high) {
    uint32_t now = now_s();
    if (!y1_on) {
      if (now - y1_last_change >= MIN_OFF_SEC) { want_Y1 = true; want_G = true; hvacAction = "cooling"; }
      else { compressorBlocked = true; }
    } else {
      want_Y1 = true; want_G = true; hvacAction = "cooling";
    }
  }

  // HEAT demand
  if ((hvacMode == M_HEAT || hvacMode == M_HEATCOOL) && currentTempF < low) {
    want_W1 = true;
    if (FAN_WITH_HEAT) want_G = true;
    if ((targetTempF - currentTempF) >= STAGE2_DELTA_F) {
      if (w_call_start == 0) w_call_start = now_s();
      if (now_s() - w_call_start >= STAGE2_DELAY_SEC) want_W2 = true;
    } else {
      w_call_start = (w1_on || w2_on) ? w_call_start : 0;
    }
    hvacAction = "heating";
  } else {
    w_call_start = 0;
  }

  // FAN-ONLY
  if (hvacMode == M_FANONLY) { want_G = true; hvacAction = "fan"; }

  // Compressor min ON/OFF enforcement
  uint32_t now = now_s();
  if (want_Y1 != y1_on) {
    if (want_Y1) {
      if (now - y1_last_change >= MIN_OFF_SEC) { setRelay(PIN_Y1, true); y1_on = true; y1_last_change = now; }
      else { compressorBlocked = true; }
    } else {
      if (now - y1_last_change >= MIN_ON_SEC) { setRelay(PIN_Y1, false); y1_on = false; y1_last_change = now; }
      else { want_Y1 = true; } // keep ON to satisfy min-on
    }
  }

  // Heat relays
  setRelay(PIN_W1, want_W1);
  setRelay(PIN_W2, want_W2);
  w1_on = want_W1; w2_on = want_W2;

  // Fan relay (on with cooling or explicit)
  bool final_G = want_G || want_Y1;
  setRelay(PIN_G, final_G);

  updateLed(want_Y1, want_W1, want_W2, final_G, compressorBlocked);
}

// --------------------- Command handling ---------------------
void handleAmbient(const JsonVariant& j) {
  if (j.containsKey("temp_f"))   currentTempF = j["temp_f"].as<float>();
  if (j.containsKey("humidity")) humidity     = j["humidity"].as<float>();
}

void handleCmd(const JsonVariant& j) {
  if (j.containsKey("mode")) {
    String m = j["mode"].as<String>(); m.toLowerCase();
    if      (m == "off")       { hvacMode = M_OFF;      hvacModeStr = "off";       allOff(); setLedOff(); }
    else if (m == "heat")      { hvacMode = M_HEAT;     hvacModeStr = "heat";      }
    else if (m == "cool")      { hvacMode = M_COOL;     hvacModeStr = "cool";      }
    else if (m == "heat_cool") { hvacMode = M_HEATCOOL; hvacModeStr = "heat_cool"; }
    else if (m == "fan_only")  { hvacMode = M_FANONLY;  hvacModeStr = "fan_only";  }
  }
  if (j.containsKey("target_temp_f")) targetTempF = j["target_temp_f"].as<float>();

  // Live tweaks
  if (j.containsKey("min_on_s"))        MIN_ON_SEC       = j["min_on_s"].as<uint32_t>();
  if (j.containsKey("min_off_s"))       MIN_OFF_SEC      = j["min_off_s"].as<uint32_t>();
  if (j.containsKey("deadband_f"))      DEADBAND_F       = j["deadband_f"].as<float>();
  if (j.containsKey("stage2_delta_f"))  STAGE2_DELTA_F   = j["stage2_delta_f"].as<float>();
  if (j.containsKey("stage2_delay_s"))  STAGE2_DELAY_SEC = j["stage2_delay_s"].as<uint32_t>();
  if (j.containsKey("fan_with_heat"))   FAN_WITH_HEAT    = j["fan_with_heat"].as<bool>();

  // Open captive portal from HA (blocks until saved/timeout)
  if (j.containsKey("portal") && j["portal"].as<bool>()) {
    runConfigPortal(false); // do not erase Wi-Fi, just open portal
  }

  // Factory Wi-Fi reset: forget credentials and reboot (will open portal on boot)
  if (j.containsKey("wifi_reset") && j["wifi_reset"].as<bool>()) {
    WiFi.disconnect(true, true); // erase NVS Wi-Fi
    prefs.begin("thermo", false);
    prefs.putString("ha_ip", "");
    prefs.putString("mqtt_host", "");
    prefs.putUShort("mqtt_port", 1883);
    prefs.putString("mqtt_user", "");
    prefs.putString("mqtt_pass", "");
    prefs.end();
    delay(300);
    ESP.restart();
  }
}

// --------------------- MQTT callback ---------------------
void onMqtt(char* topic, byte* payload, unsigned int len) {
  if (strcmp(topic, "homeassistant/status") == 0) {
    String v((char*)payload, len); v.trim();
    if (v == "online") { publishDiscovery(); publishState(); }
    return;
  }

  String t(topic);
  if (t == t_ambient || t == t_cmd) {
    StaticJsonDocument<384> d;
    if (deserializeJson(d, payload, len)) return;

    if (t == t_ambient) handleAmbient(d.as<JsonVariant>());
    else                handleCmd(d.as<JsonVariant>());

    applyOutputs();
    publishState();
  }
}

// --------------------- MQTT connect ---------------------
void ensureMqtt() {
  while (!mqtt.connected()) {
    const char* willTopic = t_avail.c_str();
    const char* willMsg   = "offline";
    bool ok = false;

    if (cfg_mqtt_user.length()) {
      ok = mqtt.connect(DEV_ID, cfg_mqtt_user.c_str(), cfg_mqtt_pass.c_str(),
                        willTopic, 1, true, willMsg);
    } else {
      ok = mqtt.connect(DEV_ID, willTopic, 1, true, willMsg);
    }

    if (ok) {
      publishAvailability("online");
      mqtt.subscribe(t_cmd.c_str());
      mqtt.subscribe(t_ambient.c_str());
      mqtt.subscribe("homeassistant/status");
      publishDiscovery();
      publishState();
    } else {
      delay(1200);
    }
  }
}

// --------------------- Config persistence ---------------------
void loadConfigFromPrefs() {
  prefs.begin("thermo", true);
  cfg_ha_ip     = prefs.getString("ha_ip", "");
  cfg_mqtt_host = prefs.getString("mqtt_host", "");
  cfg_mqtt_port = prefs.getUShort("mqtt_port", 1883);
  cfg_mqtt_user = prefs.getString("mqtt_user", "");
  cfg_mqtt_pass = prefs.getString("mqtt_pass", "");
  prefs.end();
}

void saveConfigToPrefs(const String& haip, const String& host, uint16_t port,
                       const String& user, const String& pass) {
  prefs.begin("thermo", false);
  prefs.putString("ha_ip", haip);
  prefs.putString("mqtt_host", host);
  prefs.putUShort("mqtt_port", port);
  prefs.putString("mqtt_user", user);
  prefs.putString("mqtt_pass", pass);
  prefs.end();
}

// --------------------- Captive Portal ---------------------
bool runConfigPortal(bool eraseWifi) {
  WiFiManager wm;

  wm.setConfigPortalBlocking(true);
  wm.setConfigPortalTimeout(180); // 3 minutes
  wm.setClass("invert");          // dark theme 😎

  // Prefill from saved prefs
  loadConfigFromPrefs();

  // Build param buffers
  char ha_ip[64]     = {0};
  char mqtt_host[64] = {0};
  char mqtt_port[8]  = {0};
  char mqtt_user[64] = {0};
  char mqtt_pass[64] = {0};

  strncpy(ha_ip,     cfg_ha_ip.c_str(),     sizeof(ha_ip)-1);
  strncpy(mqtt_host, cfg_mqtt_host.c_str(), sizeof(mqtt_host)-1);
  snprintf(mqtt_port, sizeof(mqtt_port), "%u", cfg_mqtt_port);
  strncpy(mqtt_user, cfg_mqtt_user.c_str(), sizeof(mqtt_user)-1);
  strncpy(mqtt_pass, cfg_mqtt_pass.c_str(), sizeof(mqtt_pass)-1);

  WiFiManagerParameter p_hint("<hr><b>Home Assistant & MQTT</b><br/>"
                              "If <i>MQTT Host</i> is left blank, the device will use the HA IP.");
  WiFiManagerParameter p_ha_ip("ha_ip", "Home Assistant IP (e.g. 192.168.50.10)", ha_ip, 63);
  WiFiManagerParameter p_mh ("mqtt_host", "MQTT Host (blank = use HA IP)", mqtt_host, 63);
  WiFiManagerParameter p_mp ("mqtt_port", "MQTT Port (default 1883)", mqtt_port, 7);
  WiFiManagerParameter p_mu ("mqtt_user", "MQTT Username (optional)", mqtt_user, 63);
  WiFiManagerParameter p_mpw("mqtt_pass", "MQTT Password (optional)", mqtt_pass, 63, "type='password'");

  wm.addParameter(&p_hint);
  wm.addParameter(&p_ha_ip);
  wm.addParameter(&p_mh);
  wm.addParameter(&p_mp);
  wm.addParameter(&p_mu);
  wm.addParameter(&p_mpw);

  if (eraseWifi) {
    WiFi.disconnect(true, true); // clear stored Wi-Fi
    delay(200);
  }

  // Try saved creds first; if not connected, open AP "ArmendaThermostat-Setup"
  bool ok = wm.autoConnect("ArmendaThermostat-Setup");
  if (!ok) {
    // Timed out or user aborted
    return false;
  }

  // Save entered params
  String new_ha_ip     = p_ha_ip.getValue();
  String new_mqtt_host = p_mh.getValue();
  String new_mqtt_port = p_mp.getValue();
  String new_mqtt_user = p_mu.getValue();
  String new_mqtt_pass = p_mpw.getValue();

  // Fallbacks
  if (new_mqtt_host.length() == 0) new_mqtt_host = new_ha_ip;
  uint16_t port = (uint16_t) (new_mqtt_port.length() ? atoi(new_mqtt_port.c_str()) : 1883);
  if (!port) port = 1883;

  saveConfigToPrefs(new_ha_ip, new_mqtt_host, port, new_mqtt_user, new_mqtt_pass);

  // Update live config
  cfg_ha_ip     = new_ha_ip;
  cfg_mqtt_host = new_mqtt_host;
  cfg_mqtt_port = port;
  cfg_mqtt_user = new_mqtt_user;
  cfg_mqtt_pass = new_mqtt_pass;

  return true;
}

// --------------------- Web Server Functions ---------------------
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1.0'>";
  html += "<title>Armenda Thermostat</title>";
  html += "<style>body{font-family:Arial,sans-serif;margin:40px;background:#f0f0f0}";
  html += ".container{background:white;padding:30px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1);max-width:800px;margin:0 auto}";
  html += "h1{color:#333;text-align:center;margin-bottom:30px}";
  html += ".status{background:#e8f5e8;padding:15px;border-left:5px solid #4CAF50;margin:20px 0}";
  html += ".controls{display:grid;grid-template-columns:1fr 1fr;gap:20px;margin:20px 0}";
  html += ".control-group{background:#f9f9f9;padding:15px;border-radius:5px}";
  html += "button{background:#4CAF50;color:white;padding:12px 20px;border:none;border-radius:4px;cursor:pointer;font-size:16px;margin:5px}";
  html += "button:hover{background:#45a049}button.secondary{background:#008CBA}button.danger{background:#f44336}";
  html += "input,select{width:100%;padding:8px;margin:5px 0;border:1px solid #ddd;border-radius:4px;box-sizing:border-box}";
  html += ".nav{text-align:center;margin:20px 0}";
  html += ".nav a{display:inline-block;margin:0 10px;padding:10px 15px;background:#008CBA;color:white;text-decoration:none;border-radius:4px}";
  html += "</style></head><body>";
  
  html += "<div class='container'>";
  html += "<h1>🌡️ Armenda Thermostat</h1>";
  
  html += "<div class='status'>";
  html += "<strong>Current Status</strong><br>";
  html += "Mode: " + hvacModeStr + " | Action: " + hvacAction + " | Target: " + String(targetTempF, 1) + "°F<br>";
  html += "Current: " + String(currentTempF, 1) + "°F | Humidity: " + String(humidity, 1) + "%<br>";
  html += "Outputs: Y1:" + String(y1_on ? "ON" : "OFF") + " W1:" + String(w1_on ? "ON" : "OFF") + 
          " W2:" + String(w2_on ? "ON" : "OFF") + "<br>";
  html += "WiFi: " + WiFi.SSID() + " (" + WiFi.localIP().toString() + ") | Uptime: " + String(now_s()) + "s";
  html += "</div>";

  html += "<div class='nav'>";
  html += "<a href='/config'>Configuration</a>";
  html += "<a href='/portal'>WiFi Setup</a>";
  html += "</div>";

  html += "<div class='controls'>";
  
  // Mode control
  html += "<div class='control-group'>";
  html += "<h3>HVAC Mode</h3>";
  html += "<form action='/setmode' method='post'>";
  html += "<select name='mode'>";
  html += "<option value='off'" + String(hvacMode == M_OFF ? " selected" : "") + ">Off</option>";
  html += "<option value='heat'" + String(hvacMode == M_HEAT ? " selected" : "") + ">Heat</option>";
  html += "<option value='cool'" + String(hvacMode == M_COOL ? " selected" : "") + ">Cool</option>";
  html += "<option value='heat_cool'" + String(hvacMode == M_HEATCOOL ? " selected" : "") + ">Auto</option>";
  html += "<option value='fan_only'" + String(hvacMode == M_FANONLY ? " selected" : "") + ">Fan Only</option>";
  html += "</select>";
  html += "<button type='submit'>Set Mode</button>";
  html += "</form>";
  html += "</div>";

  // Temperature control
  html += "<div class='control-group'>";
  html += "<h3>Target Temperature</h3>";
  html += "<form action='/settemp' method='post'>";
  html += "<input type='number' name='temp' value='" + String(targetTempF, 1) + "' step='0.5' min='55' max='85'>";
  html += "<button type='submit'>Set Temperature</button>";
  html += "</form>";
  html += "</div>";

  html += "</div>";

  // Manual sensor update
  html += "<div class='control-group'>";
  html += "<h3>Manual Sensor Update</h3>";
  html += "<form action='/setsensors' method='post'>";
  html += "Temperature (°F): <input type='number' name='temp_f' value='" + String(currentTempF, 1) + "' step='0.1'><br>";
  html += "Humidity (%): <input type='number' name='humidity' value='" + String(humidity, 1) + "' step='0.1'><br>";
  html += "<button type='submit'>Update Sensors</button>";
  html += "</form>";
  html += "</div>";

  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleConfig() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1.0'>";
  html += "<title>Armenda Thermostat - Configuration</title>";
  html += "<style>body{font-family:Arial,sans-serif;margin:40px;background:#f0f0f0}";
  html += ".container{background:white;padding:30px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1);max-width:800px;margin:0 auto}";
  html += "h1,h2{color:#333}input{width:100%;padding:8px;margin:5px 0;border:1px solid #ddd;border-radius:4px;box-sizing:border-box}";
  html += "button{background:#4CAF50;color:white;padding:12px 20px;border:none;border-radius:4px;cursor:pointer;font-size:16px;margin:5px}";
  html += "button:hover{background:#45a049}.form-group{margin:15px 0;padding:15px;background:#f9f9f9;border-radius:5px}";
  html += ".nav{text-align:center;margin:20px 0}.nav a{display:inline-block;margin:0 10px;padding:10px 15px;background:#008CBA;color:white;text-decoration:none;border-radius:4px}";
  html += "</style></head><body>";
  
  html += "<div class='container'>";
  html += "<h1>🔧 Configuration</h1>";
  html += "<div class='nav'><a href='/'>← Back to Home</a></div>";

  html += "<form action='/saveconfig' method='post'>";
  
  html += "<div class='form-group'>";
  html += "<h2>HVAC Parameters</h2>";
  html += "<label>Min Compressor On Time (seconds):</label>";
  html += "<input type='number' name='min_on_s' value='" + String(MIN_ON_SEC) + "'>";
  html += "<label>Min Compressor Off Time (seconds):</label>";
  html += "<input type='number' name='min_off_s' value='" + String(MIN_OFF_SEC) + "'>";
  html += "<label>Temperature Deadband (°F):</label>";
  html += "<input type='number' name='deadband_f' value='" + String(DEADBAND_F) + "' step='0.1'>";
  html += "<label>Stage 2 Heat Delta (°F):</label>";
  html += "<input type='number' name='stage2_delta_f' value='" + String(STAGE2_DELTA_F) + "' step='0.1'>";
  html += "<label>Stage 2 Heat Delay (seconds):</label>";
  html += "<input type='number' name='stage2_delay_s' value='" + String(STAGE2_DELAY_SEC) + "'>";
  html += "<label><input type='checkbox' name='fan_with_heat' " + String(FAN_WITH_HEAT ? "checked" : "") + "> Run fan with heat</label>";
  html += "</div>";

  html += "<button type='submit'>Save Configuration</button>";
  html += "</form>";
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleSetMode() {
  if (server.hasArg("mode")) {
    String m = server.arg("mode"); m.toLowerCase();
    if      (m == "off")       { hvacMode = M_OFF;      hvacModeStr = "off";       allOff(); setLedOff(); }
    else if (m == "heat")      { hvacMode = M_HEAT;     hvacModeStr = "heat";      }
    else if (m == "cool")      { hvacMode = M_COOL;     hvacModeStr = "cool";      }
    else if (m == "heat_cool") { hvacMode = M_HEATCOOL; hvacModeStr = "heat_cool"; }
    else if (m == "fan_only")  { hvacMode = M_FANONLY;  hvacModeStr = "fan_only";  }
    
    applyOutputs();
    publishState();
  }
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleSetTemp() {
  if (server.hasArg("temp")) {
    targetTempF = server.arg("temp").toFloat();
    applyOutputs();
    publishState();
  }
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleSetSensors() {
  if (server.hasArg("temp_f")) {
    currentTempF = server.arg("temp_f").toFloat();
  }
  if (server.hasArg("humidity")) {
    humidity = server.arg("humidity").toFloat();
  }
  applyOutputs();
  publishState();
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleSaveConfig() {
  if (server.hasArg("min_on_s")) MIN_ON_SEC = server.arg("min_on_s").toInt();
  if (server.hasArg("min_off_s")) MIN_OFF_SEC = server.arg("min_off_s").toInt();
  if (server.hasArg("deadband_f")) DEADBAND_F = server.arg("deadband_f").toFloat();
  if (server.hasArg("stage2_delta_f")) STAGE2_DELTA_F = server.arg("stage2_delta_f").toFloat();
  if (server.hasArg("stage2_delay_s")) STAGE2_DELAY_SEC = server.arg("stage2_delay_s").toInt();
  FAN_WITH_HEAT = server.hasArg("fan_with_heat");
  
  server.sendHeader("Location", "/config");
  server.send(302);
}

void handlePortal() {
  server.send(200, "text/html", "<html><body><h1>Starting WiFi Portal...</h1></body></html>");
  delay(1000);
  runConfigPortal(false);
  server.sendHeader("Location", "/");
  server.send(302);
}

void startWebServer() {
  server.on("/", handleRoot);
  server.on("/config", handleConfig);
  server.on("/setmode", HTTP_POST, handleSetMode);
  server.on("/settemp", HTTP_POST, handleSetTemp);
  server.on("/setsensors", HTTP_POST, handleSetSensors);
  server.on("/saveconfig", HTTP_POST, handleSaveConfig);
  server.on("/portal", handlePortal);
  
  server.begin();
}

// --------------------- Arduino lifecycle ---------------------
void setup() {
  // Relays
  pinMode(PIN_G,  OUTPUT);
  pinMode(PIN_W1, OUTPUT);
  pinMode(PIN_W2, OUTPUT);
  pinMode(PIN_Y1, OUTPUT);
  pinMode(PIN_R5, OUTPUT);
  pinMode(PIN_R6, OUTPUT);
  allOff();

  // LED
  led.begin();
  setLedIdle();

  // Bring up Wi-Fi; open portal if nothing saved yet
  WiFi.mode(WIFI_STA);
  loadConfigFromPrefs();

  // If we have no HA IP saved, force the portal on first boot
  bool needPortal = (cfg_ha_ip.length() == 0);
  if (needPortal) {
    for (int i=0;i<6;i++) pulsePortal();
    runConfigPortal(false);
  } else {
    // With saved config, just connect Wi-Fi
    WiFi.begin(); // use saved SSID/password
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis()-t0 < 15000) {
      pulsePortal(); // soft cyan pulse during connect
    }
    if (WiFi.status() != WL_CONNECTED) {
      // Couldn't connect → open portal
      runConfigPortal(false);
    }
  }

  // Ensure we have MQTT settings (fallback to HA IP)
  if (cfg_mqtt_host.length() == 0) cfg_mqtt_host = cfg_ha_ip;
  if (cfg_mqtt_port == 0) cfg_mqtt_port = 1883;

  // mDNS
  if (MDNS.begin("armenda-thermostat")) {
    MDNS.addService("http", "tcp", 80);
  }

  // MQTT
  mqtt.setServer(cfg_mqtt_host.c_str(), cfg_mqtt_port);
  mqtt.setCallback(onMqtt);

  // Start web server
  startWebServer();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    // Blink in portal style to hint "not connected"
    pulsePortal();
    delay(10);
    return;
  }

  setLedIdle();
  if (!mqtt.connected()) ensureMqtt();
  mqtt.loop();

  // Handle web requests
  server.handleClient();

  // Heartbeat for HA attributes
  static uint32_t t0 = 0;
  if (millis() - t0 > 5000) { t0 = millis(); publishState(); }
}
