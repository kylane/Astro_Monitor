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
//
// Wiring (I2C):
//   OLED GND -> GND
//   OLED VCC -> 3V3
//   OLED SCL -> GPIO5 (D1)
//   OLED SDA -> GPIO4 (D2)
// =============================================================================

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Config — edit config.h with your WiFi credentials and location
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
char      initTime[12] = "";    // e.g. "2026062918"

// Screen rotation
const uint8_t NUM_SCREENS = 5;
uint8_t screen = 0;
uint32_t lastScreenChange = 0;

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
  HTTPClient http;

  char url[160];
  snprintf(url, sizeof(url),
    "https://www.7timer.info/bin/astro.php?lon=%.1f&lat=%.1f&ac=0&unit=metric&output=json&tzshift=0",
    HOME_LON, HOME_LAT);

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

// Score → GO / MARGINAL / NO GO label
const char* goNoGo(int score) {
  if (score >= 85) return "PERFECT";
  if (score >= 65) return "STILL A GO";
  if (score >= 45) return "OK";
  if (score >= 25) return "DOUBTFUL";
  return "NO GO";
}

const char* goNoGoSub(int score) {
  if (score >= 85) return "Conditions are perfect";
  if (score >= 65) return "Conditions are good";
  if (score >= 45) return "Conditions are ok";
  if (score >= 25) return "Conditions aren't good";
  return "Conditions are terrible";
}

// ---------------------------------------------------------------------------
// Screen 1: TONITE — overall score + best window
// ---------------------------------------------------------------------------
void screenTonite() {
  drawHeader("TONITE");

  if (!dataValid) {
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(4, 36, "FETCHING...");  // TONITE: loading message, Y=36
    return;
  }

  // Score the next slot (closest to now) for current conditions
  int curScore = (slotCount > 0) ? calcScore(slots[0]) : 0;
  const char* verdict = goNoGo(curScore);
  const char* sub     = goNoGoSub(curScore);

  // Score bar near top
  drawBar(4, 12, 120, 6, curScore, 100);       // TONITE: score bar 0-100, Y=12

  // Big verdict below bar
  u8g2.setFont(u8g2_font_7x14B_tr);
  int vw = u8g2.getStrWidth(verdict);
  u8g2.drawStr((128 - vw) / 2, 30, verdict);   // TONITE: verdict text large centred, Y=30

  // Subtitle
  u8g2.setFont(u8g2_font_5x7_tr);
  int sw = u8g2.getStrWidth(sub);
  u8g2.drawStr((128 - sw) / 2, 40, sub);        // TONITE: subtitle centred, Y=40

  // Current slot details
  if (slotCount > 0) {
    u8g2.setFont(u8g2_font_5x7_tr);
    char l[32];
    snprintf(l, sizeof(l), "CLD:%d  SEE:%d  TRN:%d",
             slots[0].cloudcover, slots[0].seeing, slots[0].transparency);
    u8g2.drawStr(2, 50, l);              // TONITE: cloud/seeing/transparency ratings, Y=50
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
    u8g2.drawStr(2, 57, l);             // TONITE: best imaging window time + score, Y=57
    snprintf(l, sizeof(l), "%s", cloudText(slots[best].cloudcover));
    u8g2.drawStr(2, 63, l);             // TONITE: cloud description at best window, Y=63
  } else {
    u8g2.drawStr(2, 57, "No good window tonight"); // TONITE: no good window message, Y=57
  }
}

// ---------------------------------------------------------------------------
// Screen 2: CLOUDS — cloud cover bar chart for next ~18h
// ---------------------------------------------------------------------------
void screenClouds() {
  drawHeader("CLOUDS");

  if (!dataValid || slotCount == 0) {
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(4, 36, "NO DATA");     // CLOUDS: no-data message, Y=36
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
  u8g2.drawStr(axisX + 2, barY - barMaxH + 5, "CLR"); // CLOUDS: top label, 1px gap from line
  u8g2.drawStr(axisX + 2, barY,               "OVC"); // CLOUDS: bottom label, 1px gap from line
}

// ---------------------------------------------------------------------------
// Screen 3: SEEING — seeing + transparency ratings for next 6 slots
// ---------------------------------------------------------------------------
void screenSeeing() {
  drawHeader("SEEING");

  if (!dataValid || slotCount == 0) {
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(4, 36, "NO DATA");     // SEEING: no-data message, Y=36
    return;
  }

  // Current slot — big seeing rating
  u8g2.setFont(u8g2_font_7x14B_tr);
  char sv[3];
  snprintf(sv, sizeof(sv), "%d", slots[0].seeing);
  u8g2.drawStr(2, 24, sv);              // SEEING: current seeing value large, X=2 Y=24
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(16, 24, "/8");           // SEEING: out-of-8 label, X=16 Y=24
  u8g2.drawStr(30, 24, seeingText(slots[0].seeing)); // SEEING: seeing text, X=30 Y=24

  // Transparency
  u8g2.setFont(u8g2_font_7x14B_tr);
  char tv[3];
  snprintf(tv, sizeof(tv), "%d", slots[0].transparency);
  u8g2.drawStr(2, 38, tv);              // SEEING: current transparency value large, X=2 Y=38
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(16, 38, "/8");           // SEEING: out-of-8 label, X=16 Y=38
  u8g2.drawStr(30, 38, transText(slots[0].transparency)); // SEEING: transparency text, X=30 Y=38

  // Lifted index
  u8g2.setFont(u8g2_font_5x7_tr);
  char li[12];
  snprintf(li, sizeof(li), "LFT IDX: %+d", slots[0].liftedindex);
  u8g2.drawStr(2, 50, li);             // SEEING: lifted index (stability), X=2 Y=50
  const char* stab = (slots[0].liftedindex >= 2)  ? "STABLE" :
                     (slots[0].liftedindex >= -2)  ? "NEUTRAL" : "UNSTABLE";
  u8g2.drawStr(80, 50, stab);          // SEEING: stability label, X=80 Y=50

  // Mini seeing trend for next slots
  u8g2.setFont(u8g2_font_4x6_tr);
  u8g2.drawStr(2, 62, "SEE:");          // SEEING: trend label, X=2 Y=62
  int tx = 26;
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
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(4, 36, "NO DATA");     // CONDTNS: no-data message, Y=36
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
    u8g2.setFont(u8g2_font_6x12_tr);
    u8g2.drawStr(4, 36, "NO DATA");    // FORECAST: no-data message, Y=36
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

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t wt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wt < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    u8g2.clearBuffer();
    u8g2.drawStr(4, 30, "WIFI FAILED");
    u8g2.sendBuffer();
    Serial.println("\n[ASTRO] WiFi failed");
    while (true) delay(1000);
  }

  Serial.printf("\n[ASTRO] WiFi connected: %s\n", WiFi.localIP().toString().c_str());

  // Sync time
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  setenv("TZ", TIMEZONE, 1);
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

  fetchAstro();
  lastFetch = millis();
  lastScreenChange = millis();
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
  // Screen rotation
  if (millis() - lastScreenChange >= SCREEN_DWELL_MS) {
    screen = (screen + 1) % NUM_SCREENS;
    lastScreenChange = millis();
  }

  // Periodic data refresh
  if (millis() - lastFetch >= FETCH_INTERVAL_MS) {
    fetchAstro();
    lastFetch = millis();
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
