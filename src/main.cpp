#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>            // ESP32 built-in HTTP server
#include <ArduinoJson.h>
#include <time.h>

// ===== Relay Pins/Logic =====
static const int PIN_LED    = 25; // CH1
static const int PIN_FEEDER = 26; // CH2
static const int PIN_HEATER = 27; // CH3
static const int PIN_PUMP   = 33; // CH4
static const bool RELAY_ACTIVE_LOW = true; // Most relays are active-LOW

inline void relayWritePin(int pin, bool on) {
  int level = RELAY_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW);
  digitalWrite(pin, level);
}

// ===== Polling Settings =====
const unsigned long POLL_INTERVAL_MS = 15UL * 1000UL; // 1 minute
unsigned long lastPoll = 0;

// ===== Network/oneM2M Settings =====
const char* WIFI_SSID     = "your_id";  // Replace with your Wi-Fi SSID
const char* WIFI_PASSWORD = "your_password"; // Replace with your Wi-Fi password

// Mobius connection (Server certificate CN/SAN must match)
const char* MOBIUS_BASE = "https://yourIP:443";

// CSE/Resources
const char* CSEBASE   = "Mobius";
const char* AE_CTRL   = "AE-Actuator";   // Example control AE (Assumed created in advance)
const char* CNT_LED   = "LED";       // 4 containers (control command reception)
const char* CNT_FEED  = "feed";
const char* CNT_HEAT  = "heater";
const char* CNT_PUMP  = "pump";

// oneM2M common
String X_M2M_Origin = "SM";     // Adjust to match the server ACP (Recommended to match AE name)
unsigned long reqId  = 10000;

// Root CA (Server certificate issuing CA must match)
static const char root_ca_pem[] = R"EOF(
-----BEGIN CERTIFICATE-----

-----END CERTIFICATE-----
)EOF";

// ===== Internal HTTP Server (Notify Reception) =====
WebServer server(8080); // Port 8080

// ===== TLS Client =====
WiFiClientSecure secureClient;

// ===== Utilities =====
String makeUrl(const String& path) {
  String base = MOBIUS_BASE;
  if (base.endsWith("/")) base.remove(base.length()-1);
  return base + "/" + path;
}

void setCommonHeaders(HTTPClient& http, bool isPost, int ty) {
  http.addHeader("Accept", "application/json");
  if (isPost) http.addHeader("Content-Type", "application/json; ty=" + String(ty));
  http.addHeader("X-M2M-Origin", X_M2M_Origin);
  http.addHeader("X-M2M-RI", String(reqId++));
  http.addHeader("X-M2M-RVI", "4");
}

// KST=UTC+9 (TLS verification)
bool syncTimeWithNTP(uint32_t timeout_ms = 10000) {
  configTime(9*3600, 0, "pool.ntp.org", "time.nist.gov");
  uint32_t start = millis();
  time_t now;
  do {
    delay(200);
    time(&now);
    if (now > 1700000000) {
      Serial.printf("Time synced: %ld\n", now);
      return true;
    }
  } while (millis() - start < timeout_ms);
  Serial.println("NTP sync timeout");
  return false;
}

// =========================
// Create Subscription and auto-correct nu
// =========================
bool createSubscription(const char* targetCnt, const char* subRn, const char* endpointPath) {
  HTTPClient http;
  String ip = WiFi.localIP().toString();
  String nu = "http://" + ip + ":8080/" + endpointPath;

  String target = makeUrl(String(CSEBASE) + "/" + AE_CTRL + "/" + targetCnt);
  Serial.printf("[SUB] %-6s -> POST %s (nu=%s)\n", targetCnt, target.c_str(), nu.c_str());

  if (!http.begin(secureClient, target)) {
    Serial.printf("[SUB] http.begin failed: %s\n", target.c_str());
    return false;
  }

  setCommonHeaders(http, true, 23);
  String body = String("{\"m2m:sub\":{") +
                "\"rn\":\"" + String(subRn) + "\"," +
                "\"enc\":{\"net\":[3]}," +      // Create child CIN event
                "\"nct\":2," +                  // whole resource
                "\"nu\":[\"" + nu + "\"]" +
                "}}";

  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  Serial.printf("[SUB] %-6s -> HTTP %d\n", targetCnt, code);
  if (resp.length()) Serial.printf("[SUB] Resp: %s\n", resp.c_str());

  if (code == 201) return true;        // Created
  if (code == 409) {                   // Already exists —> Check/Correct nu
    Serial.printf("[SUB] Already exists (409): %s\n", subRn);
    // Simple correction: If the current IP is not in the existing SUB's nu, replace it with PUT
    HTTPClient g;
    String subPath = String(CSEBASE) + "/" + AE_CTRL + "/" + targetCnt + "/" + subRn;
    String getUrl = makeUrl(subPath);

    if (g.begin(secureClient, getUrl)) {
      g.addHeader("Accept", "application/json");
      g.addHeader("X-M2M-Origin", X_M2M_Origin);
      g.addHeader("X-M2M-RI", String(reqId++));
      g.addHeader("X-M2M-RVI", "4");
      int gc = g.GET();
      String gr = g.getString();
      g.end();
      if (gc == 200) {
        if (gr.indexOf(ip) < 0) {
          // If current IP is not found, replace nu
          HTTPClient u;
          if (u.begin(secureClient, getUrl)) {
            u.addHeader("Accept", "application/json");
            u.addHeader("Content-Type", "application/json");
            u.addHeader("X-M2M-Origin", X_M2M_Origin);
            u.addHeader("X-M2M-RI", String(reqId++));
            u.addHeader("X-M2M-RVI", "4");
            String putBody = String("{\"m2m:sub\":{\"nu\":[\"") + nu + "\"]}}";
            int uc = u.PUT(putBody);
            String ur = u.getString();
            u.end();
            Serial.printf("[SUB][PUT] %s -> HTTP %d\n", subRn, uc);
            if (ur.length()) Serial.println(ur);
          }
        } else {
          Serial.printf("[SUB] nu already up-to-date for %s\n", subRn);
        }
      }
    }
    return true;
  }
  return false;
}

// =========================
// Notify Parser/Handler
// =========================
bool parseConToOnOff(const String& con, bool& outOn) {
  String s = con; s.trim();
  if (s.equalsIgnoreCase("on")  || s == "1") { outOn = true;  return true; }
  if (s.equalsIgnoreCase("off") || s == "0") { outOn = false; return true; }

  if (s.length() > 0 && s[0] == '{') {
    StaticJsonDocument<256> d;
    if (deserializeJson(d, s) == DeserializationError::Ok) {
      if (d.containsKey("cmd")) {
        const char* cmd = d["cmd"];
        if (cmd) {
          if (!strcasecmp(cmd, "on"))  { outOn = true;  return true; }
          if (!strcasecmp(cmd, "off")) { outOn = false; return true; }
        }
      }
      if (d.containsKey("on")) {
        if (d["on"].is<bool>()) { outOn = d["on"].as<bool>(); return true; }
        if (d["on"].is<int>())  { outOn = d["on"].as<int>() != 0; return true; }
        if (d["on"].is<const char*>()) {
          const char* v = d["on"];
          if (!strcasecmp(v, "on"))  { outOn = true;  return true; }
          if (!strcasecmp(v, "off")) { outOn = false; return true; }
        }
      }
    }
  }
  return false;
}

// Mobius Notify Body - Extract "con" and "ri" strings
bool extractConRiFromNotify(const String& body, String& outCon, String& outRi) {
  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return false;

  JsonVariant sgn = doc["m2m:sgn"]; if (sgn.isNull()) sgn = doc["sgn"];
  if (sgn.isNull()) return false;

  JsonVariant nev = sgn["nev"]; if (nev.isNull()) return false;
  JsonVariant rep = nev["rep"]; if (rep.isNull()) return false;
  JsonVariant cin = rep["m2m:cin"]; if (cin.isNull()) return false;

  JsonVariant con = cin["con"]; if (con.isNull()) return false;
  outCon = con.as<String>(); outCon.trim();
  if (outCon.indexOf("\\\"") >= 0) { String un=outCon; un.replace("\\\"", "\""); un.replace("\\\\","\\"); if (un.startsWith("{")&&un.endsWith("}")) outCon=un; }

  JsonVariant ri = cin["ri"]; if (!ri.isNull()) outRi = ri.as<String>(); else outRi = "";
  return true;
}

// =========================
// FEEDER Specific: 2-second Pulse State Machine
// =========================
const unsigned long FEED_PULSE_MS = 2000;
bool feederPulseActive = false;
unsigned long feederPulseEndMs = 0;
String lastProcessedFeederRi = ""; // Prevent duplicate triggers

void startFeederPulse() {
  // If pulse is already active, leave it as is, otherwise start new pulse (optional logic change)
  if (!feederPulseActive) {
    feederPulseActive = true;
    feederPulseEndMs = millis() + FEED_PULSE_MS;
    relayWritePin(PIN_FEEDER, true); // ON
    Serial.println("[FEEDER] PULSE START (2s)");
  } else {
    // If pulse is already running and another "on" comes, update the end time
    // feederPulseEndMs = millis() + FEED_PULSE_MS;
  }
}

void feederPulseService() {
  if (feederPulseActive && millis() >= feederPulseEndMs) {
    relayWritePin(PIN_FEEDER, false); // OFF
    feederPulseActive = false;
    Serial.println("[FEEDER] PULSE END");
  }
}

// =========================
// Notify Handler (LED/HEATER/PUMP regular, FEEDER special)
// =========================
void handleNotifyAndDrivePin(int pin, const char* name) {
  String body = server.arg("plain");
  if (body.isEmpty()) { server.send(400, "text/plain", "empty"); return; }

  // Regular channel: existing logic
  String con;
  if (!extractConRiFromNotify(body, con, *(new String()))) { // ri is ignored
    server.send(400, "text/plain", "no con");
    Serial.printf("[NOTIFY][%s] invalid payload\n", name);
    return;
  }
  bool on = false;
  if (!parseConToOnOff(con, on)) {
    server.send(400, "text/plain", "bad con");
    Serial.printf("[NOTIFY][%s] con parse fail: %s\n", name, con.c_str());
    return;
  }
  relayWritePin(pin, on);
  server.send(200, "text/plain", "ok");
  Serial.printf("[NOTIFY][%s] %s (con=%s)\n", name, on ? "ON" : "OFF", con.c_str());
}

void handle_n_led()    { handleNotifyAndDrivePin(PIN_LED,    "LED"); }
void handle_n_heater() { handleNotifyAndDrivePin(PIN_HEATER, "HEATER"); }
void handle_n_pump()   { handleNotifyAndDrivePin(PIN_PUMP,   "PUMP"); }

// FEEDER: on means 2-second pulse, off means ignored + ri duplicate prevention
void handle_n_feeder() {
  String body = server.arg("plain");
  if (body.isEmpty()) { server.send(400, "text/plain", "empty"); return; }

  String con, ri;
  if (!extractConRiFromNotify(body, con, ri)) {
    server.send(400, "text/plain", "no con/ri");
    Serial.println("[NOTIFY][FEEDER] invalid payload");
    return;
  }
  bool on=false;
  if (!parseConToOnOff(con, on)) {
    server.send(400, "text/plain", "bad con");
    Serial.printf("[NOTIFY][FEEDER] con parse fail: %s\n", con.c_str());
    return;
  }

  // dismiss in case of ri duplicate
  if (ri.length() && ri == lastProcessedFeederRi) {
    server.send(200, "text/plain", "dup");
    return;
  }

  if (on) {
    lastProcessedFeederRi = ri;
    startFeederPulse();
    server.send(200, "text/plain", "ok");
    Serial.printf("[NOTIFY][FEEDER] TRIGGER (ri=%s)\n", ri.c_str());
  } else {
    // off
    server.send(200, "text/plain", "ignored");
    Serial.println("[NOTIFY][FEEDER] ignored(off)");
  }
}

// =========================
// 1minute polling (LED/HEATER/PUMP)
// =========================
bool fetchLatestAndDrive(const char* cnt, int pin, const char* name) {
  HTTPClient http;
  String target = makeUrl(String(CSEBASE) + "/" + AE_CTRL + "/" + cnt + "/la");

  if (!http.begin(secureClient, target)) {
    Serial.printf("[POLL][%s] begin fail: %s\n", name, target.c_str());
    return false;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("X-M2M-Origin", X_M2M_Origin);
  http.addHeader("X-M2M-RI", String(reqId++));
  http.addHeader("X-M2M-RVI", "4");

  int code = http.GET();
  String resp = http.getString();
  http.end();

  if (code == 200) {
    StaticJsonDocument<1024> doc;
    if (deserializeJson(doc, resp) == DeserializationError::Ok) {
      JsonVariant cin = doc["m2m:cin"];
      if (!cin.isNull()) {
        String con = cin["con"].as<String>(); con.trim();
        if (con.indexOf("\\\"") >= 0) { String un=con; un.replace("\\\"", "\""); un.replace("\\\\","\\"); if (un.startsWith("{")&&un.endsWith("}")) con=un; }
        bool on=false;
        if (parseConToOnOff(con, on)) {
          relayWritePin(pin, on);
          Serial.printf("[POLL][%s] %s (con=%s)\n", name, on ? "ON" : "OFF", con.c_str());
          return true;
        } else {
          Serial.printf("[POLL][%s] con parse fail: %s\n", name, con.c_str());
        }
      }
    } else {
      Serial.printf("[POLL][%s] JSON parse error\n", name);
    }
    return false;
  }
  if (code == 404) { Serial.printf("[POLL][%s] latest not found (404)\n", name); return false; }
  Serial.printf("[POLL][%s] HTTP %d\n", name, code);
  if (resp.length()) Serial.println(resp);
  return false;
}

// FEEDER polling: confirm ri
bool fetchLatestFeederAndMaybePulse() {
  HTTPClient http;
  String target = makeUrl(String(CSEBASE) + "/" + AE_CTRL + "/" + CNT_FEED + "/la");

  if (!http.begin(secureClient, target)) {
    Serial.printf("[POLL][FEEDER] begin fail: %s\n", target.c_str());
    return false;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("X-M2M-Origin", X_M2M_Origin);
  http.addHeader("X-M2M-RI", String(reqId++));
  http.addHeader("X-M2M-RVI", "4");

  int code = http.GET();
  String resp = http.getString();
  http.end();

  if (code == 200) {
    StaticJsonDocument<2048> doc;
    if (deserializeJson(doc, resp) == DeserializationError::Ok) {
      JsonVariant cin = doc["m2m:cin"];
      if (!cin.isNull()) {
        String ri = cin["ri"].as<String>();
        String con = cin["con"].as<String>(); con.trim();
        if (con.indexOf("\\\"") >= 0) { String un=con; un.replace("\\\"", "\""); un.replace("\\\\","\\"); if (un.startsWith("{")&&un.endsWith("}")) con=un; }
        bool on=false;
        if (parseConToOnOff(con, on)) {
          if (on && ri != lastProcessedFeederRi) {
            lastProcessedFeederRi = ri;
            startFeederPulse();
            Serial.printf("[POLL][FEEDER] TRIGGER (ri=%s)\n", ri.c_str());
          } else if (!on) {
            // off
            Serial.println("[POLL][FEEDER] ignored(off)");
          }
          return true;
        } else {
          Serial.printf("[POLL][FEEDER] con parse fail: %s\n", con.c_str());
        }
      }
    } else {
      Serial.println("[POLL][FEEDER] JSON parse error");
    }
    return false;
  }
  if (code == 404) { Serial.println("[POLL][FEEDER] latest not found (404)"); return false; }
  Serial.printf("[POLL][FEEDER] HTTP %d\n", code);
  if (resp.length()) Serial.println(resp);
  return false;
}

// =========================
// SETUP / LOOP
// =========================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[Actuator] Booting...");

  // Reset Relay to Safe State
  pinMode(PIN_LED,    OUTPUT); relayWritePin(PIN_LED,    false);
  pinMode(PIN_FEEDER, OUTPUT); relayWritePin(PIN_FEEDER, false);
  pinMode(PIN_HEATER, OUTPUT); relayWritePin(PIN_HEATER, false);
  pinMode(PIN_PUMP,   OUTPUT); relayWritePin(PIN_PUMP,   false);

  // Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi connecting...");
  while (WiFi.status() != WL_CONNECTED) { Serial.print("."); delay(400); }
  Serial.printf("\nWiFi connected: %s\n", WiFi.localIP().toString().c_str());

  // TLS
  syncTimeWithNTP();
  secureClient.setCACert(root_ca_pem);

  // Internal HTTP Server
  server.on("/n_led",    HTTP_ANY, handle_n_led);
  server.on("/n_feeder", HTTP_ANY, handle_n_feeder);
  server.on("/n_heater", HTTP_ANY, handle_n_heater);
  server.on("/n_pump",   HTTP_ANY, handle_n_pump);
  server.begin();
  Serial.println("[Actuator] HTTP server started on :8080");

  // Subscription setting
  bool ok1 = createSubscription(CNT_LED,  "sub_led",    "n_led");
  bool ok2 = createSubscription(CNT_FEED, "sub_feeder", "n_feeder");
  bool ok3 = createSubscription(CNT_HEAT, "sub_heater", "n_heater");
  bool ok4 = createSubscription(CNT_PUMP, "sub_pump",   "n_pump");
  Serial.printf("[SUB RESULT] led=%d feeder=%d heater=%d pump=%d\n", ok1, ok2, ok3, ok4);

  // polling once immediately after boot
  fetchLatestAndDrive(CNT_LED,  PIN_LED,    "LED");
  fetchLatestFeederAndMaybePulse();
  fetchLatestAndDrive(CNT_HEAT, PIN_HEATER, "HEATER");
  fetchLatestAndDrive(CNT_PUMP, PIN_PUMP,   "PUMP");
}

void loop() {
  // Notify reception process
  server.handleClient();

  // FEEDER pulse state
  feederPulseService();

  // 1minute polling
  unsigned long now = millis();
  if (now - lastPoll >= POLL_INTERVAL_MS) {
    lastPoll = now;
    fetchLatestAndDrive(CNT_LED,  PIN_LED,    "LED");
    fetchLatestFeederAndMaybePulse(); // feeder
    fetchLatestAndDrive(CNT_HEAT, PIN_HEATER, "HEATER");
    fetchLatestAndDrive(CNT_PUMP, PIN_PUMP,   "PUMP");
  }

  delay(5);
}
