// =============================================================================
// Astro Sky Conditions Monitor
// ESP8266 + 1.3" SH1106 I2C OLED
//
// Uses 7timer.info "astro" product (no API key needed, HTTP only)
// Rotates through 5 screens showing astronomy-relevant conditions.
//
// Libraries (install from Arduino Library Manager):
//   - U8g2        by olikraus
//   - ArduinoJson by Benoit Blanchon (v7.x)
//   - WiFiManager by tzapu
//
// Wiring (I2C):
//   OLED GND -> GND
//   OLED VCC -> 3V3
//   OLED SCL -> GPIO5 (D1)
//   OLED SDA -> GPIO4 (D2)
//
// WiFi / location setup:
//   On first boot (or if WiFi can't connect), the device opens a setup
//   portal AP called "AstroMonitor-Setup". Connect to it and a captive
//   portal page lets you pick your WiFi network and enter latitude,
//   longitude and POSIX timezone.
//   To reopen the portal later (e.g. to change location or WiFi), press
//   the board's FLASH button (GPIO0) at any time while it's running —
//   release quickly to just open the portal, or hold 5+ seconds for a
//   full factory reset (wipes WiFi + location/timezone, then restarts).
// =============================================================================

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Config — edit config.h for default location/timezone and display timing.
// WiFi credentials are no longer stored here — see setup portal above.
// ---------------------------------------------------------------------------
#include "config.h"

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
#define PIN_OLED_RST  16
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, PIN_OLED_RST);

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

// One 3-hourly forecast slot from 7timer astro product
struct AstroSlot {
  int  timepoint;     // hours from init time
  int  cloudcover;    // 1–9  (1=clear, 9=overcast)
  int  seeing;        // 1–8  (1=bad, 8=excellent)
  int  transparency;  // 1–8  (1=bad, 8=excellent)
  int  liftedindex;   // stability: negative = unstable
  int  rh2m;          // relative humidity %
  int  windspd;       // wind speed (7timer scale 1–8)
  char winddir[4];    // e.g. "SW"
  int  temp2m;        // temperature °C
  char prectype[8];   // "none", "rain", "snow", etc.
};

const uint8_t MAX_SLOTS = 16;   // up to 48 hours ahead
AstroSlot slots[MAX_SLOTS];
uint8_t   slotCount = 0;
bool      dataValid = false;
uint32_t  lastFetch = 0;
uint16_t  fetchAttempts = 0;    // consecutive attempts since the last success
char      initTime[12] = "";    // e.g. "2026062918"

// Screen rotation
const uint8_t NUM_SCREENS = 5;
uint8_t screen = 0;
uint32_t lastScreenChange = 0;

// ---------------------------------------------------------------------------
// Runtime settings — lat/lon/timezone/location name/rotation time, editable
// via the setup portal and persisted to LittleFS. WiFi credentials are
// persisted separately by WiFiManager/the ESP8266 SDK itself.
// ---------------------------------------------------------------------------
float homeLat = HOME_LAT;
float homeLon = HOME_LON;
char  tzString[64];
char  locationName[40] = "Location Unknown";
uint32_t screenDwellMs = SCREEN_DWELL_MS;

void loadSettings() {
  strncpy(tzString, TIMEZONE, sizeof(tzString) - 1);
  tzString[sizeof(tzString) - 1] = '\0';

  if (!LittleFS.begin()) {
    Serial.println("[CFG] LittleFS mount failed, using defaults");
    return;
  }
  if (!LittleFS.exists("/settings.json")) return;

  File f = LittleFS.open("/settings.json", "r");
  if (!f) return;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("[CFG] settings.json parse error: %s\n", err.c_str());
    return;
  }

  homeLat = doc["lat"] | HOME_LAT;
  homeLon = doc["lon"] | HOME_LON;
  strncpy(tzString, doc["tz"] | TIMEZONE, sizeof(tzString) - 1);
  tzString[sizeof(tzString) - 1] = '\0';
  strncpy(locationName, doc["loc"] | "Location Unknown", sizeof(locationName) - 1);
  locationName[sizeof(locationName) - 1] = '\0';
  screenDwellMs = doc["dwell"] | SCREEN_DWELL_MS;
  Serial.printf("[CFG] Loaded lat=%.5f lon=%.5f tz=%s loc=%s dwell=%lums\n",
                homeLat, homeLon, tzString, locationName, screenDwellMs);
}

void saveSettings(float lat, float lon, const char* tz, const char* loc, uint32_t dwellMs) {
  JsonDocument doc;
  doc["lat"]   = lat;
  doc["lon"]   = lon;
  doc["tz"]    = tz;
  doc["loc"]   = loc;
  doc["dwell"] = dwellMs;

  File f = LittleFS.open("/settings.json", "w");
  if (!f) {
    Serial.println("[CFG] Failed to open settings.json for write");
    return;
  }
  serializeJson(doc, f);
  f.close();
  Serial.println("[CFG] Settings saved to LittleFS");
}

// ---------------------------------------------------------------------------
// Helpers: scale descriptions
// ---------------------------------------------------------------------------

// cloudcover 1-9 → text
const char* cloudText(int c) {
  if (c <= 1) return "CLEAR";
  if (c <= 2) return "MOSTLY CLR";
  if (c <= 3) return "FEW CLOUDS";
  if (c <= 5) return "PARTLY CLDY";
  if (c <= 7) return "MOSTLY CLDY";
  return "OVERCAST";
}

// seeing 1-8 → text
const char* seeingText(int s) {
  if (s <= 1) return "TERRIBLE";
  if (s <= 2) return "BAD";
  if (s <= 3) return "POOR";
  if (s <= 4) return "BELOW AVG";
  if (s <= 5) return "AVERAGE";
  if (s <= 6) return "GOOD";
  if (s <= 7) return "VERY GOOD";
  return "EXCELLENT";
}

// transparency 1-8 → text
const char* transText(int t) {
  if (t <= 1) return "TERRIBLE";
  if (t <= 2) return "BAD";
  if (t <= 3) return "BELOW AVG";
  if (t <= 4) return "AVERAGE";
  if (t <= 5) return "ABOVE AVG";
  if (t <= 6) return "GOOD";
  if (t <= 7) return "VERY GOOD";
  return "EXCELLENT";
}

// wind speed scale 1-8 → km/h approximate
int windKmh(int spd) {
  // 7timer wind scale: 1=<5, 2=5-9, 3=10-16, 4=17-24, 5=25-32, 6=33-40, 7=41-47, 8=>47
  const int tbl[] = {0, 3, 7, 13, 20, 28, 36, 44, 50};
  if (spd < 1) spd = 1;
  if (spd > 8) spd = 8;
  return tbl[spd];
}

// ---------------------------------------------------------------------------
// Go/no-go scoring — returns 0 (bad) to 100 (perfect)
// ---------------------------------------------------------------------------
int calcScore(const AstroSlot& s) {
  // Cloud cover is king — 1=best, 9=worst
  int cloudScore = map(constrain(s.cloudcover, 1, 9), 1, 9, 100, 0);
  // Seeing 1-8, higher=better
  int seeingScore = map(constrain(s.seeing, 1, 8), 1, 8, 0, 100);
  // Transparency 1-8, higher=better
  int transScore = map(constrain(s.transparency, 1, 8), 1, 8, 0, 100);
  // Lifted index — 0 or positive is stable, negative is bad
  int liScore = (s.liftedindex >= 0) ? 100 : map(constrain(s.liftedindex, -10, 0), -10, 0, 0, 100);
  // Precipitation — automatic 0 if raining
  if (strcmp(s.prectype, "none") != 0) return 0;
  // Weighted average
  return (cloudScore * 50 + seeingScore * 25 + transScore * 15 + liScore * 10) / 100;
}

// Find the best nighttime slot (18:00–06:00 local) in next 24h
// Returns index into slots[], or -1 if none
int bestNightSlot() {
  if (!dataValid || slotCount == 0) return -1;
  int bestIdx = -1;
  int bestScore = -1;
  // Use local time to determine which timepoints fall in tonight's window
  time_t now = time(nullptr);
  for (uint8_t i = 0; i < slotCount; i++) {
    time_t slotTime = now + (slots[i].timepoint * 3600L);
    struct tm lt;
    localtime_r(&slotTime, &lt);
    int h = lt.tm_hour;
    bool isNight = (h >= 20 || h < 5);   // 8pm–5am
    if (!isNight) continue;
    int score = calcScore(slots[i]);
    if (score > bestScore) {
      bestScore = score;
      bestIdx = i;
    }
  }
  return bestIdx;
}

// ---------------------------------------------------------------------------
// Fetch from 7timer — with retry for malformed JSON
// ---------------------------------------------------------------------------
bool fetchAstro() {
  WiFiClientSecure client;
  client.setInsecure();   // skip certificate validation — fine for public weather API
  client.setTimeout(8000);  // bound the TCP/TLS socket itself — see fetchLocationName() note
  HTTPClient http;
  http.setTimeout(8000);   // bound the HTTP-layer read too, so a stalled request always
                           // returns (and gets retried) instead of hanging forever

  char url[160];
  snprintf(url, sizeof(url),
    "https://www.7timer.info/bin/astro.php?lon=%.1f&lat=%.1f&ac=0&unit=metric&output=json&tzshift=0",
    homeLon, homeLat);

  Serial.printf("[ASTRO] Fetching: %s\n", url);

  for (int attempt = 1; attempt <= FETCH_RETRIES; attempt++) {
    http.begin(client, url);
    int code = http.GET();
    if (code != 200) {
      Serial.printf("[ASTRO] HTTP %d on attempt %d\n", code, attempt);
      http.end();
      delay(2000);
      continue;
    }

    String payload = http.getString();
    http.end();

    // ArduinoJson v7
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      Serial.printf("[ASTRO] JSON error on attempt %d: %s\n", attempt, err.c_str());
      delay(2000);
      continue;
    }

    // Parse init time
    const char* init = doc["init"] | "";
    strncpy(initTime, init, sizeof(initTime) - 1);

    // Parse dataseries
    JsonArray ds = doc["dataseries"].as<JsonArray>();
    slotCount = 0;
    for (JsonObject entry : ds) {
      if (slotCount >= MAX_SLOTS) break;
      AstroSlot& s = slots[slotCount];
      s.timepoint   = entry["timepoint"] | 0;
      s.cloudcover  = entry["cloudcover"]   | 5;
      s.seeing      = entry["seeing"]        | 4;
      s.transparency= entry["transparency"]  | 4;
      s.liftedindex = entry["lifted_index"]  | 0;
      s.rh2m        = entry["rh2m"]          | 50;
      s.temp2m      = entry["temp2m"]        | 15;
      JsonObject w  = entry["wind10m"];
      s.windspd     = w["speed"] | 2;
      strncpy(s.winddir, w["direction"] | "?", sizeof(s.winddir) - 1);
      strncpy(s.prectype, entry["prec_type"] | "none", sizeof(s.prectype) - 1);
      slotCount++;
    }

    if (slotCount == 0) {
      Serial.println("[ASTRO] No slots parsed, retrying");
      delay(2000);
      continue;
    }

    dataValid = true;
    Serial.printf("[ASTRO] OK — %d slots, init=%s\n", slotCount, initTime);
    return true;
  }

  Serial.println("[ASTRO] All retries failed");
  return false;
}

// Wraps fetchAstro() with the attempt counter that drives retry cadence and
// on-screen messaging (see drawNoDataMessage() and the loop() refresh logic).
void doFetchAstro() {
  if (fetchAstro()) {
    fetchAttempts = 0;
  } else {
    fetchAttempts++;
  }
  lastFetch = millis();
}

// ---------------------------------------------------------------------------
// Resolve a human place name from lat/lon via BigDataCloud's free reverse
// geocoding endpoint (no API key required, no signup). Only called once per
// location change — the result is cached in settings.json afterwards.
// ---------------------------------------------------------------------------
bool fetchLocationName(float lat, float lon) {
  WiFiClientSecure client;
  client.setInsecure();   // skip certificate validation — fine for a public geocoding API
  client.setTimeout(8000);  // bound the TCP/TLS socket — without this, a stalled
                            // connection can block setup() forever, freezing the
                            // whole device (screen rotation, everything) since
                            // nothing after this call ever gets to run
  HTTPClient http;
  http.setTimeout(8000);   // bound the HTTP-layer read too

  char url[192];
  snprintf(url, sizeof(url),
    "https://api.bigdatacloud.net/data/reverse-geocode-client?latitude=%.5f&longitude=%.5f&localityLanguage=en",
    lat, lon);

  Serial.printf("[GEO] Fetching: %s\n", url);
  http.begin(client, url);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[GEO] HTTP %d\n", code);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[GEO] JSON error: %s\n", err.c_str());
    return false;
  }

  const char* city     = doc["city"]                | "";
  const char* locality = doc["locality"]             | "";
  const char* region   = doc["principalSubdivision"] | "";
  const char* country  = doc["countryName"]          | "";

  const char* best = city[0] ? city : (locality[0] ? locality : (region[0] ? region : country));
  if (!best || !best[0]) {
    Serial.println("[GEO] No usable place name in response");
    return false;
  }

  strncpy(locationName, best, sizeof(locationName) - 1);
  locationName[sizeof(locationName) - 1] = '\0';
  Serial.printf("[GEO] Resolved: %s\n", locationName);
  return true;
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

void drawHeader(const char* title) {
  u8g2.setFont(u8g2_font_5x7_tr);
  char left[22];
  snprintf(left, sizeof(left), "%s %d/%d", title, screen + 1, NUM_SCREENS);
  u8g2.drawStr(2, 6, left);               // HEADER: title + screen index, X=2 Y=6

  // clock (right-aligned)
  char t[10];
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  strftime(t, sizeof(t), "%H:%M:%S", &lt);
  u8g2.drawStr(127 - u8g2.getStrWidth(t), 6, t); // HEADER: clock right-aligned, Y=6

  u8g2.drawHLine(0, 8, 128);             // HEADER: divider line, Y=8
  u8g2.drawVLine(0, 0, 3);              // HEADER: top-left corner tick
  u8g2.drawVLine(125, 0, 3);            // HEADER: top-right corner tick
}

// Small bar rating, width proportional to val/maxVal, at (x,y) w*h pixels
void drawBar(int x, int y, int w, int h, int val, int maxVal) {
  u8g2.drawFrame(x, y, w, h);
  int fill = map(constrain(val, 0, maxVal), 0, maxVal, 0, w - 2);
  if (fill > 0) u8g2.drawBox(x + 1, y + 1, fill, h - 2);
}

// Shown on any screen while dataValid is false. Message depends on how many
// fetch attempts have happened since the last success: a brief note while
// still in the fast-retry window (every 10s, up to 10 tries), then longer
// guidance once that's expired and retries have backed off to every 60s.
void drawNoDataMessage() {
  if (fetchAttempts < 10) {
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(4, 36, "Refreshing data");
  } else {
    u8g2.setFont(u8g2_font_5x7_tr);
    const char* l1 = "Retrying...";
    const char* l2 = "Try a power cycle";
    const char* l3 = "if this persists";
    u8g2.drawStr((128 - u8g2.getStrWidth(l1)) / 2, 28, l1);
    u8g2.drawStr((128 - u8g2.getStrWidth(l2)) / 2, 40, l2);
    u8g2.drawStr((128 - u8g2.getStrWidth(l3)) / 2, 50, l3);
  }
}

// Score → GO / MARGINAL / NO GO label
const char* goNoGo(int score) {
  if (score >= 85) return "PERFECT";
  if (score >= 65) return "GOOD ENOUGH";
  if (score >= 45) return "MARGINAL";
  if (score >= 25) return "DOUBTFUL";
  return "TERRIBLE";
}

const char* goNoGoSub(int score) {
  if (score >= 85) return "Conditions are perfect";
  if (score >= 65) return "Conditions are good";
  if (score >= 45) return "Conditions are ok at best";
  if (score >= 25) return "Conditions are not great";
  return "Conditions are terrible";
}

// ---------------------------------------------------------------------------
// Screen 1: TONITE — overall score + best window
// ---------------------------------------------------------------------------
void screenTonite() {
  drawHeader("TONITE");

  // Location name, centred, right under the header divider
  u8g2.setFont(u8g2_font_4x6_tr);
  int lw = u8g2.getStrWidth(locationName);
  u8g2.drawStr((128 - lw) / 2, 15, locationName); // TONITE: resolved place name, Y=15

  if (!dataValid) {
    drawNoDataMessage();
    return;
  }

  // Score the next slot (closest to now) for current conditions
  int curScore = (slotCount > 0) ? calcScore(slots[0]) : 0;
  const char* verdict = goNoGo(curScore);
  const char* sub     = goNoGoSub(curScore);

  // Score bar near top
  drawBar(4, 18, 120, 5, curScore, 100);       // TONITE: score bar 0-100, Y=18

  // Big verdict below bar
  u8g2.setFont(u8g2_font_7x14B_tr);
  int vw = u8g2.getStrWidth(verdict);
  u8g2.drawStr((128 - vw) / 2, 36, verdict);   // TONITE: verdict text large centred, Y=36

  // Subtitle
  u8g2.setFont(u8g2_font_5x7_tr);
  int sw = u8g2.getStrWidth(sub);
  u8g2.drawStr((128 - sw) / 2, 44, sub);        // TONITE: subtitle centred, Y=44

  // Current slot details
  if (slotCount > 0) {
    u8g2.setFont(u8g2_font_5x7_tr);
    char l[32];
    snprintf(l, sizeof(l), "CLD:%d  SEE:%d  TRN:%d",
             slots[0].cloudcover, slots[0].seeing, slots[0].transparency);
    u8g2.drawStr(2, 52, l);              // TONITE: cloud/seeing/transparency ratings, Y=52
  }

  // Best tonight window
  int best = bestNightSlot();
  u8g2.setFont(u8g2_font_4x6_tr);
  if (best >= 0) {
    time_t now = time(nullptr);
    time_t slotTime = now + (slots[best].timepoint * 3600L);
    struct tm lt;
    localtime_r(&slotTime, &lt);
    char l[32];
    snprintf(l, sizeof(l), "BEST WINDOW: %02d:00  SCR:%d",
             lt.tm_hour, calcScore(slots[best]));
    u8g2.drawStr(2, 58, l);             // TONITE: best imaging window time + score, Y=58
    snprintf(l, sizeof(l), "%s", cloudText(slots[best].cloudcover));
    u8g2.drawStr(2, 63, l);             // TONITE: cloud description at best window, Y=63
  } else {
    u8g2.drawStr(2, 58, "No good window tonight"); // TONITE: no good window message, Y=58
  }
}

// ---------------------------------------------------------------------------
// Screen 2: CLOUDS — cloud cover bar chart for next ~18h
// ---------------------------------------------------------------------------
void screenClouds() {
  drawHeader("CLOUDS");

  if (!dataValid || slotCount == 0) {
    drawNoDataMessage();
    return;
  }

  // Show up to 6 timepoints as vertical bar chart
  const uint8_t BARS = min((int)slotCount, 6);
  const int barW    = 16;
  const int barMaxH = 32;
  const int barY    = 44;              // moved down 6px from header
  const int startX  = 2;
  const int axisX   = startX + BARS * (barW + 1) + 2; // 1px gap after last bar

  for (uint8_t i = 0; i < BARS; i++) {
    int x = startX + i * (barW + 1);
    int barH = map(slots[i].cloudcover, 1, 9, 2, barMaxH);
    u8g2.drawFrame(x, barY - barMaxH, barW, barMaxH); // CLOUDS: bar outline
    u8g2.drawBox(x + 1, barY - barH + 1, barW - 2, barH - 2); // CLOUDS: cloud fill
    // Hour label
    u8g2.setFont(u8g2_font_5x7_tr);
    time_t now = time(nullptr);
    time_t slotTime = now + (slots[i].timepoint * 3600L);
    struct tm lt;
    localtime_r(&slotTime, &lt);
    char h[6];
    snprintf(h, sizeof(h), "%02dh", lt.tm_hour);
    u8g2.drawStr(x + 1, barY + 10, h);   // CLOUDS: hour label, Y=barY+10
    // Cloud cover rating
    char cld[6];
    snprintf(cld, sizeof(cld), "%d/9", slots[i].cloudcover);
    u8g2.setFont(u8g2_font_4x6_tr);
    u8g2.drawStr(x + 2, barY + 19, cld); // CLOUDS: cloud cover "N/9", Y=barY+19
  }

  // Y-axis: 1px gap after last bar, 1px gap before labels
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawVLine(axisX, barY - barMaxH, barMaxH);              // CLOUDS: Y-axis line
  u8g2.drawStr(axisX + 2, barY - barMaxH + 5, "OVC"); // CLOUDS: top label, 1px gap from line
  u8g2.drawStr(axisX + 2, barY,               "CLR"); // CLOUDS: bottom label, 1px gap from line
}

// ---------------------------------------------------------------------------
// Screen 3: SEEING — seeing + transparency ratings for next 6 slots
// ---------------------------------------------------------------------------
void screenSeeing() {
  drawHeader("SEE RATING");

  if (!dataValid || slotCount == 0) {
    drawNoDataMessage();
    return;
  }

  // Current slot — big seeing rating
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(2, 24, "SEE");            // SEEING: row label, X=2 Y=24
  u8g2.setFont(u8g2_font_7x14B_tr);
  char sv[3];
  snprintf(sv, sizeof(sv), "%d", slots[0].seeing);
  u8g2.drawStr(46, 24, sv);              // SEEING: current seeing value large, X=46 Y=24
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(55, 24, "/8");            // SEEING: out-of-8 label, X=55 Y=24
  u8g2.drawStr(69, 24, seeingText(slots[0].seeing)); // SEEING: seeing text, X=69 Y=24

  // Transparency
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(2, 38, "TRANSPCY");       // SEEING: row label, X=2 Y=38
  u8g2.setFont(u8g2_font_7x14B_tr);
  char tv[3];
  snprintf(tv, sizeof(tv), "%d", slots[0].transparency);
  u8g2.drawStr(46, 38, tv);              // SEEING: current transparency value large, X=46 Y=38
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(55, 38, "/8");            // SEEING: out-of-8 label, X=55 Y=38
  u8g2.drawStr(69, 38, transText(slots[0].transparency)); // SEEING: transparency text, X=69 Y=38

  // Lifted index
  u8g2.setFont(u8g2_font_5x7_tr);
  char li[16];
  snprintf(li, sizeof(li), "STABILITY: %+d", slots[0].liftedindex);
  u8g2.drawStr(2, 50, li);             // SEEING: lifted index (stability), X=2 Y=50
  const char* stab = (slots[0].liftedindex >= 2)  ? "STABLE" :
                     (slots[0].liftedindex >= -2)  ? "NEUTRAL" : "UNSTABLE";
  u8g2.drawStr(80, 50, stab);          // SEEING: stability label, X=80 Y=50

  // Mini seeing trend for next slots
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(2, 62, "TREND:");        // SEEING: trend label, X=2 Y=62
  int tx = 30;
  for (uint8_t i = 0; i < min((int)slotCount, 5); i++) {
    char s[3];
    snprintf(s, sizeof(s), "%d", slots[i].seeing);
    u8g2.drawStr(tx, 62, s);            // SEEING: seeing trend values, Y=62
    tx += 18;
  }
}

// ---------------------------------------------------------------------------
// Screen 4: CONDITIONS — wind, humidity, temp, precip
// ---------------------------------------------------------------------------
void screenConditions() {
  drawHeader("CONDTNS");

  if (!dataValid || slotCount == 0) {
    drawNoDataMessage();
    return;
  }

  AstroSlot& s = slots[0];

  u8g2.setFont(u8g2_font_6x12_tr);

  // Temperature
  char l[32];
  snprintf(l, sizeof(l), "TEMP  %d\xB0" "C", s.temp2m);
  u8g2.drawStr(2, 22, l);              // CONDTNS: temperature, X=2 Y=22

  // Wind
  snprintf(l, sizeof(l), "WIND  %s %dkm/h", s.winddir, windKmh(s.windspd));
  u8g2.drawStr(2, 34, l);             // CONDTNS: wind direction + speed, X=2 Y=34

  // Humidity
  snprintf(l, sizeof(l), "HUM   %d%%", s.rh2m);
  u8g2.drawStr(2, 46, l);             // CONDTNS: relative humidity, X=2 Y=46

  // Precipitation
  bool raining = strcmp(s.prectype, "none") != 0;
  snprintf(l, sizeof(l), "PREC  %s", raining ? s.prectype : "NONE");
  u8g2.drawStr(2, 58, l);             // CONDTNS: precipitation type, X=2 Y=58
}

// ---------------------------------------------------------------------------
// Screen 5: FORECAST — 24h outlook strip with scores
// ---------------------------------------------------------------------------
void screenForecast() {
  drawHeader("FORECAST");

  if (!dataValid || slotCount == 0) {
    drawNoDataMessage();
    return;
  }

  // 3 rows at 5x7 font for readability
  const uint8_t ROWS = min((int)slotCount, 3);
  u8g2.setFont(u8g2_font_5x7_tr);

  // Column headers
  u8g2.drawStr(2,  16, "TIME");        // FORECAST: column header - time, Y=16
  u8g2.drawStr(38, 16, "CLD");         // FORECAST: column header - cloud, Y=16
  u8g2.drawStr(58, 16, "SEE");         // FORECAST: column header - seeing, Y=16
  u8g2.drawStr(78, 16, "TRN");         // FORECAST: column header - transparency, Y=16
  u8g2.drawStr(100,16, "GO?");         // FORECAST: column header - go/no-go, Y=16
  u8g2.drawHLine(0, 18, 128);          // FORECAST: header divider, Y=18

  for (uint8_t i = 0; i < ROWS; i++) {
    int y = 30 + i * 13;               // 13px row spacing for 5x7 font

    time_t now = time(nullptr);
    time_t slotTime = now + (slots[i].timepoint * 3600L);
    struct tm lt;
    localtime_r(&slotTime, &lt);

    char row[8];
    snprintf(row, sizeof(row), "%02d:00", lt.tm_hour);
    u8g2.drawStr(2,  y, row);          // FORECAST: slot time, Y=y

    snprintf(row, sizeof(row), "%d", slots[i].cloudcover);
    u8g2.drawStr(42, y, row);          // FORECAST: cloud cover value, Y=y

    snprintf(row, sizeof(row), "%d", slots[i].seeing);
    u8g2.drawStr(62, y, row);          // FORECAST: seeing value, Y=y

    snprintf(row, sizeof(row), "%d", slots[i].transparency);
    u8g2.drawStr(82, y, row);          // FORECAST: transparency value, Y=y

    int score = calcScore(slots[i]);
    bool raining = strcmp(slots[i].prectype, "none") != 0;
    const char* go = raining ? "RAIN" : (score >= 85 ? "GO!" : score >= 65 ? "GO" : score >= 45 ? "OK" : score >= 25 ? "DBT" : "NO");
    u8g2.drawStr(100, y, go);          // FORECAST: go/no-go label, Y=y
  }
}

// ---------------------------------------------------------------------------
// WiFi/location setup portal. Runs at boot (forcePortal=false: only opens if
// there's no saved WiFi or it can't connect) or on demand from loop() when
// the FLASH button is pressed (forcePortal=true: always opens).
//
// GPIO0 must NOT be held low across an actual power-on/reset — the ESP8266's
// boot ROM samples it at that exact moment and, if low, enters the UART
// flash/download bootloader instead of running this sketch at all (which is
// why the display used to stay completely blank when holding the button
// while powering up). So the button is only ever read here, well after
// boot has already completed normally, via loop()'s continuous polling.
//
// If the user saves new settings, we show a confirmation and reboot the
// device ourselves (ESP.restart()) rather than asking them to power cycle —
// this guarantees NTP/timezone/geocode/forecast all re-initialize cleanly
// with the new values, and this function never returns in that case.
//
// If the submitted WiFi credentials fail to connect, WiFiManager's default
// behaviour is to silently keep retrying inside the same blocking call with
// no visible feedback on our OLED — setBreakAfterConfig(true) makes it
// return to us after any submission (success or failure) instead, so we can
// show a clear "couldn't connect, reopening" message and let the user try
// again immediately rather than staring at a frozen "SETUP MODE" screen.
// ---------------------------------------------------------------------------
bool runWifiSetup(bool forcePortal) {
  char latStr[16], lonStr[16], dwellStr[8];
  snprintf(latStr, sizeof(latStr), "%.5f", homeLat);
  snprintf(lonStr, sizeof(lonStr), "%.5f", homeLon);
  snprintf(dwellStr, sizeof(dwellStr), "%lu", (unsigned long)(screenDwellMs / 1000));

  WiFiManagerParameter html_latlon_link(
    "<p style='margin:8px 0 2px'>Find your <b>latitude/longitude</b> at "
    "<a href='https://www.latlong.net' target='_blank'>latlong.net</a><br>"
    "<small>(this link may not load once you're connected to this WiFi — if so, "
    "look it up beforehand, write it down, and come back to enter it below)</small></p>");
  WiFiManagerParameter custom_lat("lat", "Latitude", latStr, sizeof(latStr) - 1);
  WiFiManagerParameter custom_lon("lon", "Longitude", lonStr, sizeof(lonStr) - 1);
  WiFiManagerParameter html_tz_link(
    "<p style='margin:8px 0 2px'>Find your <b>POSIX timezone string</b> in "
    "<a href='https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv' target='_blank'>this list</a><br>"
    "<small>(this link may not load once you're connected to this WiFi — if so, "
    "look it up beforehand, write it down, and come back to enter it below)</small></p>");
  WiFiManagerParameter custom_tz("tz", "POSIX Timezone", tzString, sizeof(tzString) - 1);
  WiFiManagerParameter custom_dwell("dwell", "Screen rotation time (seconds)", dwellStr, sizeof(dwellStr) - 1);

  bool settingsSaved = false;
  WiFiManager wm;
  wm.addParameter(&html_latlon_link);
  wm.addParameter(&custom_lat);
  wm.addParameter(&custom_lon);
  wm.addParameter(&html_tz_link);
  wm.addParameter(&custom_tz);
  wm.addParameter(&custom_dwell);
  wm.setSaveConfigCallback([&settingsSaved]() { settingsSaved = true; });
  wm.setAPCallback([](WiFiManager*) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(4, 16, "SETUP MODE");
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(4, 32, "Join WiFi:");
    u8g2.drawStr(4, 42, "AstroMonitor-Setup");
    u8g2.drawStr(4, 56, "Then open 192.168.4.1");
    u8g2.sendBuffer();
  });
  wm.setConfigPortalTimeout(300);     // give up and continue after 5 min
  wm.setBreakAfterConfig(true);       // return to us even if the connect attempt fails

  bool connected = false;
  bool firstAttempt = true;
  while (true) {
    settingsSaved = false;
    connected = (forcePortal || !firstAttempt)
      ? wm.startConfigPortal("AstroMonitor-Setup")
      : wm.autoConnect("AstroMonitor-Setup");
    firstAttempt = false;

    if (!settingsSaved) break;   // nothing submitted (timed out / abandoned) — stop
    if (connected) break;        // submitted and connected — fall through to apply it

    // Submitted, but the new WiFi credentials didn't connect — say so and
    // reopen the portal for another attempt instead of silently retrying.
    Serial.println("[CFG] WiFi connect failed after portal submit — reopening setup");
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(4, 18, "WIFI FAILED");
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(4, 34, "Could not connect to");
    u8g2.drawStr(4, 44, "that network.");
    u8g2.drawStr(4, 56, "Reopening setup...");
    u8g2.sendBuffer();
    delay(3000);
    forcePortal = true;   // make sure the retry reopens the portal
  }

  if (settingsSaved && connected) {
    homeLat = atof(custom_lat.getValue());
    homeLon = atof(custom_lon.getValue());
    strncpy(tzString, custom_tz.getValue(), sizeof(tzString) - 1);
    tzString[sizeof(tzString) - 1] = '\0';

    long dwellSec = atol(custom_dwell.getValue());
    if (dwellSec < 2) dwellSec = 2;       // keep rotation sane at either extreme
    if (dwellSec > 120) dwellSec = 120;
    screenDwellMs = (uint32_t)dwellSec * 1000UL;

    // Location changed — old resolved name no longer applies until re-geocoded
    strncpy(locationName, "Location Unknown", sizeof(locationName) - 1);
    locationName[sizeof(locationName) - 1] = '\0';
    saveSettings(homeLat, homeLon, tzString, locationName, screenDwellMs);

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(4, 24, "SETTINGS SAVED");
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(4, 40, "Restarting to apply");
    u8g2.drawStr(4, 50, "new settings...");
    u8g2.sendBuffer();
    Serial.println("[CFG] Settings saved — restarting");
    delay(2500);
    ESP.restart();
    // never reached
  }

  if (!connected) {
    // Whatever WiFi state we had before is likely disrupted by the portal's
    // AP+STA mode; drop back to plain STA so the caller's own connectivity
    // checks behave predictably.
    WiFi.mode(WIFI_STA);
  }

  return connected;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("\n[ASTRO] Booting...");

  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(4, 20, "ASTRO MONITOR");
  u8g2.drawStr(4, 36, "Connecting WiFi");
  u8g2.sendBuffer();

  loadSettings();

  // FLASH button (GPIO0) is only ever polled from loop() — see the note on
  // runWifiSetup() for why it must not be read/held during boot itself.
  pinMode(0, INPUT_PULLUP);

  bool connected = runWifiSetup(false);

  if (!connected) {
    u8g2.clearBuffer();
    u8g2.drawStr(4, 30, "WIFI FAILED");
    u8g2.sendBuffer();
    Serial.println("\n[ASTRO] WiFi failed");
    while (true) delay(1000);
  }

  Serial.printf("\n[ASTRO] WiFi connected: %s\n", WiFi.localIP().toString().c_str());

  // Coming out of the setup portal leaves the radio in AP+STA mode (the
  // portal runs its own AP alongside the STA connection). Force pure STA
  // and give the network stack — DNS in particular — a moment to settle
  // before making any HTTPS requests. Skipping this is why the very first
  // fetch right after a portal save can fail until the next full power
  // cycle: a cold boot never goes through an AP+STA transition, so it
  // doesn't hit this window.
  WiFi.mode(WIFI_STA);
  delay(1500);

  // Sync time
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  setenv("TZ", tzString, 1);
  tzset();
  Serial.print("[ASTRO] Syncing NTP");
  uint32_t nt = millis();
  while (time(nullptr) < 1000000 && millis() - nt < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" done");

  u8g2.clearBuffer();
  u8g2.drawStr(4, 20, "ASTRO MONITOR");
  u8g2.drawStr(4, 36, "Fetching data...");
  u8g2.sendBuffer();

  // Weather fetch runs before the (non-essential) geocode lookup below —
  // back-to-back HTTPS/TLS requests right after boot can strain the
  // ESP8266's limited RAM, and the forecast is the actual point of this
  // device, so it gets first crack at a clean memory state.
  doFetchAstro();
  lastScreenChange = millis();

  // Resolve a human-readable place name once per location change (or once
  // on first boot if it's never been resolved) — cached in settings.json
  // afterwards so this isn't called on every boot.
  if (strcmp(locationName, "Location Unknown") == 0) {
    Serial.println("[GEO] Resolving location name...");
    if (fetchLocationName(homeLat, homeLon)) {
      saveSettings(homeLat, homeLon, tzString, locationName, screenDwellMs);
    } else {
      Serial.println("[GEO] Lookup failed, using \"Location Unknown\"");
    }
  }
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
  // FLASH button (GPIO0): press any time to open the setup portal (release
  // quickly), or keep holding 5+ seconds for a factory reset that wipes the
  // saved WiFi credentials and settings.json, then restarts into a blank
  // portal. See runWifiSetup() for why this is only ever checked here, in
  // loop(), rather than at boot.
  if (digitalRead(0) == LOW) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(4, 16, "Button held");
    u8g2.drawStr(4, 30, "Release now: SETUP");
    u8g2.drawStr(4, 42, "Keep holding 5s for:");
    u8g2.drawStr(4, 52, "FACTORY RESET");
    u8g2.sendBuffer();

    uint32_t holdStart = millis();
    while (digitalRead(0) == LOW && millis() - holdStart < 5000) {
      delay(50);
    }

    if (millis() - holdStart >= 5000) {
      Serial.println("[CFG] Factory reset requested — erasing WiFi + settings");
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x12_tr);
      u8g2.drawStr(4, 28, "FACTORY RESET");
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(4, 44, "Erasing + restarting");
      u8g2.sendBuffer();

      WiFiManager wm;
      wm.resetSettings();                // erase saved WiFi credentials
      LittleFS.remove("/settings.json"); // erase saved location/timezone
      delay(1500);
      ESP.restart();
      // never reached
    } else {
      // Short press — open the portal now. If the user saves, runWifiSetup()
      // restarts the device itself and never returns. If they back out or it
      // times out, we just fall through and resume normal operation below.
      runWifiSetup(true);
    }
  }

  // Screen rotation
  if (millis() - lastScreenChange >= screenDwellMs) {
    screen = (screen + 1) % NUM_SCREENS;
    lastScreenChange = millis();
  }

  // Periodic data refresh. While we don't have valid data yet, retry every
  // 10 seconds for the first 10 attempts (fast recovery from a bad
  // boot-time fetch), then back off to every 60 seconds — the on-screen
  // message switches at that point to suggest a power cycle if it's still
  // not resolving on its own.
  uint32_t retryInterval;
  if (dataValid) {
    retryInterval = FETCH_INTERVAL_MS;
  } else if (fetchAttempts < 10) {
    retryInterval = 10000UL;
  } else {
    retryInterval = 60000UL;
  }
  if (millis() - lastFetch >= retryInterval) {
    doFetchAstro();
  }

  // Draw
  u8g2.clearBuffer();
  switch (screen) {
    case 0: screenTonite();     break;  // Overall go/no-go + best window
    case 1: screenClouds();     break;  // Cloud cover bar chart
    case 2: screenSeeing();     break;  // Seeing + transparency
    case 3: screenConditions(); break;  // Wind, humidity, temp, precip
    case 4: screenForecast();   break;  // 24h table
  }
  u8g2.sendBuffer();

  delay(100);
}
