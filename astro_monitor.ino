// =============================================================================
// Astro Sky Conditions Monitor
// ESP32-C6 (Waveshare ESP32-C6-LCD-1.47) + 1.47" ST7789 SPI color TFT, 172x320
// rotated to a 320x172 landscape framebuffer. Onboard WS2812 status LED
// mirrors the TONITE screen's verdict color.
//
// Uses 7timer.info "astro" product (no API key needed, HTTP only)
// Rotates through 7 screens showing astronomy-relevant conditions.
//
// Libraries (install from Arduino Library Manager):
//   - GFX Library for Arduino ("Arduino_GFX") by moononournation
//   - U8g2         by olikraus     (used only for its bundled font data)
//   - Adafruit NeoPixel by Adafruit (drives the onboard WS2812 status LED)
//   - ArduinoJson  by Benoit Blanchon (v7.x)
//   - WiFiManager  by tzapu
//
// Wiring (SPI, write-only — no MISO):
//   LCD GND  -> GND
//   LCD VCC  -> 3V3
//   LCD MOSI -> GPIO6
//   LCD SCLK -> GPIO7
//   LCD CS   -> GPIO14
//   LCD DC   -> GPIO15
//   LCD RST  -> GPIO21
//   LCD BLK  -> GPIO22
//   RGB LED  -> GPIO8 (onboard WS2812, single pixel)
// (On the Waveshare ESP32-C6-LCD-1.47 dev board these are already wired
//  on-board — no external wiring needed, this is for reference only.)
//
// WiFi / location setup:
//   On first boot (or if WiFi can't connect), the device opens a setup
//   portal AP called "AstroMonitor-Setup". Connect to it and a captive
//   portal page lets you pick your WiFi network and enter latitude,
//   longitude, POSIX timezone and your Bortle scale rating (light
//   pollution, 1-9 — looked up manually, see runWifiSetup()).
//   The board's BOOT button (GPIO9) is used at any time while it's running:
//   single click jumps to the TONITE screen, double-click reopens this
//   setup portal (e.g. to change location or WiFi), and holding 5+ seconds
//   triggers a full factory reset (wipes WiFi + location/timezone, then
//   restarts).
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <U8g2lib.h>
#include <Arduino_GFX_Library.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>   // sin/cos/atan/acos/fmod — sunrise/sunset calc (see calcSunEvent())

// ---------------------------------------------------------------------------
// Config — edit config.h for default location/timezone and display timing.
// WiFi credentials are no longer stored here — see setup portal above.
// ---------------------------------------------------------------------------
#include "config.h"

// ---------------------------------------------------------------------------
// Display — ST7789, 172x320 native panel, rotated to a 320x172 landscape
// framebuffer. Driven through Arduino_Canvas so each frame is composed in
// RAM and pushed to the panel in one SPI burst (flush()), avoiding the
// flicker a partial/direct-to-panel redraw would cause on a color TFT.
// ---------------------------------------------------------------------------
#define PIN_LCD_MOSI  6
#define PIN_LCD_SCLK  7
#define PIN_LCD_CS    14
#define PIN_LCD_DC    15
#define PIN_LCD_RST   21
#define PIN_LCD_BL    22
#define PIN_BOOT_BTN  9   // BOOT button — ESP32-C6's strapping pin, plays the
                          // same role GPIO0 played on the ESP8266 (see the
                          // GPIO0 caveat in the old wiring notes: same rule
                          // applies here — don't hold it low across an actual
                          // power-on/reset, only ever poll it from loop()).
#define PIN_RGB_LED   8   // onboard WS2812 addressable RGB status LED

#define LCD_ROTATION  1   // 1 = landscape, 320x172. If the display comes up
                          // upside-down on your unit, change this to 3.
#define DISP_W        320
#define DISP_H        172

Arduino_DataBus *bus = new Arduino_HWSPI(
  PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_MOSI, GFX_NOT_DEFINED /* MISO unused, panel is write-only */);
Arduino_GFX *tft = new Arduino_ST7789(
  bus, PIN_LCD_RST, LCD_ROTATION, true /* IPS — this panel needs inversion on for correct (non-inverted) colors */,
  172 /* native width */, 320 /* native height */,
  34 /* col_offset1 */, 0 /* row_offset1 */, 34 /* col_offset2 */, 0 /* row_offset2 */);
Arduino_Canvas *canvas = new Arduino_Canvas(DISP_W, DISP_H, tft);

// Onboard status LED — mirrors the TONITE screen's verdict color (see
// updateStatusLed(), called whenever a fetch completes) so you can tell
// whether it's worth observing without needing the screen to be on TONITE.
Adafruit_NeoPixel rgbLed(1, PIN_RGB_LED, NEO_RGB + NEO_KHZ800);  // this LED is wired R,G,B — NEO_GRB swapped red/green

// Fonts — u8g2's bundled bitmap fonts, rendered directly by Arduino_GFX.
// FONT_MD (also used for the header) is the smallest font used anywhere in
// the UI — small/tiny tiers were tried and read as unreadable on real
// hardware, so every screen is now laid out around this as the size floor.
#define FONT_HDR  u8g2_font_9x18_tr        // header title + clock
#define FONT_MD   u8g2_font_9x18_tr        // smallest size used anywhere — primary body text
#define FONT_BOLD u8g2_font_helvB18_tr     // splash/status screens (SETUP MODE, WIFI FAILED, etc.)
#define FONT_LG   u8g2_font_logisoso24_tr  // big digit call-outs (SEE/TRANSPCY rating)
#define FONT_XL   u8g2_font_logisoso32_tr  // biggest — TONITE verdict

// Color palette
#define COL_BG      RGB565_BLACK
#define COL_TEXT    RGB565_WHITE
#define COL_DIM     RGB565_LIGHTGREY
#define COL_HEADER  RGB565_CYAN
#define COL_GOOD    RGB565_LIME
#define COL_LIME    RGB565_CHARTREUSE
#define COL_OK      RGB565_YELLOW
#define COL_WARN    RGB565_ORANGE
#define COL_BAD     RGB565_RED
#define COL_MOONDARK 0x4208               // unlit portion of the moon-phase disc (dark grey)

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

// Tonight's real sunset->sunrise window (see computeTonightWindow()) — and
// one run of consecutive clear/poor slots within it (see
// buildNightSegments()). Declared here rather than next to the functions
// that use them: the Arduino IDE hoists auto-generated function prototypes
// to the top of the file, above any type defined later, so a struct used in
// a function signature has to be declared this early or the hoisted
// prototype fails to compile.
struct NightWindow { time_t start; time_t end; };
struct NightSegment { bool clear; time_t start; };
// Moon phase/illumination for a given instant (see computeMoon()). Declared
// up here for the same reason as the two structs above — it's a function
// return type, and the IDE hoists prototypes above anything defined later.
struct MoonData {
  double age;         // days since the last new moon, 0..29.53
  double illum;       // illuminated fraction, 0..1
  bool   waxing;      // true while the lit fraction is still growing
  const char* phase;  // phase name, e.g. "Waxing Gibbous"
};

const uint8_t MAX_SLOTS = 16;   // up to 48 hours ahead
AstroSlot slots[MAX_SLOTS];
uint8_t   slotCount = 0;
bool      dataValid = false;
uint32_t  lastFetch = 0;
uint16_t  fetchAttempts = 0;    // consecutive attempts since the last success
bool      lastFetchOk = false;  // result of the most recent fetch attempt
time_t    lastFetchEpoch = 0;   // wall-clock time of the last *successful* fetch (0 = never)
char      initTime[12] = "";    // e.g. "2026062918"

// Screen rotation
const uint8_t NUM_SCREENS = 7;
uint8_t screen = 0;
uint32_t lastScreenChange = 0;

// ---------------------------------------------------------------------------
// Runtime settings — lat/lon/timezone/bortle/location name/rotation time,
// editable via the setup portal and persisted to LittleFS. WiFi credentials
// are persisted separately by WiFiManager/the ESP32 SDK itself.
// ---------------------------------------------------------------------------
float homeLat = HOME_LAT;
float homeLon = HOME_LON;
uint8_t homeBortle = HOME_BORTLE;   // 1 (darkest) - 9 (brightest); fixed site
                                    // property, not weather — see config.h
char  tzString[64];
char  locationName[40] = "Location Unknown";
uint32_t screenDwellMs = SCREEN_DWELL_MS;

void loadSettings() {
  strncpy(tzString, TIMEZONE, sizeof(tzString) - 1);
  tzString[sizeof(tzString) - 1] = '\0';

  if (!LittleFS.begin(true)) {  // true = format the partition if it's blank/corrupt
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
  homeBortle = doc["bortle"] | HOME_BORTLE;
  strncpy(tzString, doc["tz"] | TIMEZONE, sizeof(tzString) - 1);
  tzString[sizeof(tzString) - 1] = '\0';
  strncpy(locationName, doc["loc"] | "Location Unknown", sizeof(locationName) - 1);
  locationName[sizeof(locationName) - 1] = '\0';
  screenDwellMs = doc["dwell"] | SCREEN_DWELL_MS;
  Serial.printf("[CFG] Loaded lat=%.5f lon=%.5f bortle=%u tz=%s loc=%s dwell=%lums\n",
                homeLat, homeLon, homeBortle, tzString, locationName, screenDwellMs);
}

void saveSettings(float lat, float lon, uint8_t bortle, const char* tz, const char* loc, uint32_t dwellMs) {
  JsonDocument doc;
  doc["lat"]    = lat;
  doc["lon"]    = lon;
  doc["bortle"] = bortle;
  doc["tz"]     = tz;
  doc["loc"]    = loc;
  doc["dwell"]  = dwellMs;

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

// 0-100 "goodness" percentage → a traffic-light tier: 0=perfect, 1=good, 2=ok,
// 3=warn, 4=bad. Mirrors goNoGo()'s five verdict bands exactly, so a slot's
// color always matches its verdict. Shared by the screen colors
// (scoreColor()) and the onboard status LED (updateStatusLed()) so the two
// can never drift out of sync with each other.
int scoreTier(int pct) {
  if (pct >= 81) return 0;
  if (pct >= 61) return 1;
  if (pct >= 41) return 2;
  if (pct >= 21) return 3;
  return 4;
}

uint16_t scoreColor(int pct) {
  static const uint16_t tierColor[] = { COL_GOOD, COL_LIME, COL_OK, COL_WARN, COL_BAD };
  return tierColor[scoreTier(pct)];
}

// cloudcover 1-9 (1=best) and seeing/transparency 1-8 (8=best) as a 0-100
// "goodness" percentage, so they can share scoreColor()'s traffic-light bands.
int cloudPct(int c)  { return map(constrain(c, 1, 9), 1, 9, 100, 0); }
int seeingPct(int s) { return map(constrain(s, 1, 8), 1, 8, 0, 100); }

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
  // Weighted average — purely atmospheric/weather. Bortle rating is fixed
  // per site and never changes night to night, so it's shown on TONITE for
  // context only (see screenTonite()) rather than folded in here: baking a
  // constant into this average wouldn't tell you anything new about tonight
  // specifically, it would just apply the same flat offset to every night.
  return (cloudScore * 50 + seeingScore * 25 + transScore * 15 + liScore * 10) / 100;
}

// A slot counts as "clear" for trend/threshold purposes at the same cutoff
// goNoGo() uses for GOOD — below this it's rated "poor".
const int CLEAR_SCORE_CUTOFF = 61;

// ---------------------------------------------------------------------------
// Sunrise/sunset — the real dark-hours window for tonight, used below to
// decide which forecast slots actually belong to "tonight" rather than a
// fixed 20:00-05:00 guess. Standard Sunrise/Sunset equation (public domain,
// see https://edwilliams.org/sunrise_sunset_algorithm.htm); zenith 90.833
// accounts for atmospheric refraction and the sun's apparent radius (civil
// sunrise/sunset, matches what you'd call "dark" by eye).
// ---------------------------------------------------------------------------

// Days since 1970-01-01 for a proleptic-Gregorian UTC calendar date (Howard
// Hinnant's days_from_civil) — used instead of timegm(), which isn't
// guaranteed available on the ESP32 toolchain.
static long daysFromCivil(int y, int m, int d) {
  y -= (m <= 2) ? 1 : 0;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + (long)doe - 719468L;
}

// Computes sunrise or sunset (UTC epoch) for a given calendar date/location.
// Returns false if the sun doesn't rise/set that day (polar latitudes only —
// won't happen at any latitude this device is realistically deployed at, but
// cheap to guard against a NaN propagating into the score).
bool calcSunEvent(float lat, float lon, int year, int month, int day, bool sunrise, time_t& outEpoch) {
  const double zenith = 90.833;
  const double deg2rad = PI / 180.0;
  const double rad2deg = 180.0 / PI;

  int N1 = (275 * month) / 9;
  int N2 = (month + 9) / 12;
  int N3 = (1 + (year - 4 * (year / 4) + 2) / 3);
  int N = N1 - (N2 * N3) + day - 30;

  double lngHour = lon / 15.0;
  double t = sunrise ? (N + ((6 - lngHour) / 24)) : (N + ((18 - lngHour) / 24));

  double M = (0.9856 * t) - 3.289;

  double L = M + (1.916 * sin(M * deg2rad)) + (0.020 * sin(2 * M * deg2rad)) + 282.634;
  L = fmod(L + 360.0, 360.0);

  double RA = rad2deg * atan(0.91764 * tan(L * deg2rad));
  RA = fmod(RA + 360.0, 360.0);
  double Lquadrant  = floor(L / 90) * 90;
  double RAquadrant = floor(RA / 90) * 90;
  RA = (RA + (Lquadrant - RAquadrant)) / 15.0;

  double sinDec = 0.39782 * sin(L * deg2rad);
  double cosDec = cos(asin(sinDec));

  double cosH = (cos(zenith * deg2rad) - (sinDec * sin(lat * deg2rad))) / (cosDec * cos(lat * deg2rad));
  if (cosH > 1.0 || cosH < -1.0) return false;

  double H = sunrise ? (360.0 - rad2deg * acos(cosH)) : (rad2deg * acos(cosH));
  H = H / 15.0;

  double T = H + RA - (0.06571 * t) - 6.622;
  double UT = fmod(T - lngHour + 24.0, 24.0);

  outEpoch = (time_t)(daysFromCivil(year, month, day) * 86400L + (long)(UT * 3600.0));
  return true;
}

// Tonight's real sunset->sunrise window. If it's currently after midnight
// but before dawn, "tonight" is still the tail of the window that opened at
// yesterday's sunset — not a window starting at today's (upcoming) sunset.
bool computeTonightWindow(NightWindow& nw) {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);

  time_t sunriseToday, sunsetToday;
  if (!calcSunEvent(homeLat, homeLon, lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, true,  sunriseToday)) return false;
  if (!calcSunEvent(homeLat, homeLon, lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, false, sunsetToday))  return false;

  if (now < sunriseToday) {
    time_t yesterday = now - 86400L;
    struct tm ylt;
    localtime_r(&yesterday, &ylt);
    time_t sunsetYesterday;
    if (!calcSunEvent(homeLat, homeLon, ylt.tm_year + 1900, ylt.tm_mon + 1, ylt.tm_mday, false, sunsetYesterday)) return false;
    nw.start = sunsetYesterday;
    nw.end   = sunriseToday;
  } else {
    time_t tomorrow = now + 86400L;
    struct tm tlt;
    localtime_r(&tomorrow, &tlt);
    time_t sunriseTomorrow;
    if (!calcSunEvent(homeLat, homeLon, tlt.tm_year + 1900, tlt.tm_mon + 1, tlt.tm_mday, true, sunriseTomorrow)) return false;
    nw.start = sunsetToday;
    nw.end   = sunriseTomorrow;
  }
  return true;
}

bool slotInWindow(uint8_t i, const NightWindow& nw) {
  time_t st = lastFetchEpoch + (slots[i].timepoint * 3600L);
  return st >= nw.start && st < nw.end;
}

// Average score across tonight's real sunset->sunrise window — this is what
// TONITE's headline score/verdict represents, not just the nearest forecast
// slot. A single bad hour barely moves it; a sustained bad stretch (e.g.
// clouds rolling in after a clear sunset and staying) correctly drags it
// down. Returns -1 if the window can't be computed or no slots fall in it
// (e.g. stale/short forecast data) — callers fall back to the nearest slot.
int tonightAverageScore() {
  if (!dataValid || slotCount == 0) return -1;
  NightWindow nw;
  if (!computeTonightWindow(nw)) return -1;
  int sum = 0, count = 0;
  for (uint8_t i = 0; i < slotCount; i++) {
    if (!slotInWindow(i, nw)) continue;
    sum += calcScore(slots[i]);
    count++;
  }
  return (count > 0) ? (sum / count) : -1;
}

// Min/max temperature across tonight's window — used by CONDTNS to show an
// overnight range alongside the current reading. Returns false (lo/hi left
// unset) if no slots fall in the window.
bool nightTempRange(const NightWindow& nw, int& lo, int& hi) {
  bool any = false;
  for (uint8_t i = 0; i < slotCount; i++) {
    if (!slotInWindow(i, nw)) continue;
    int t = slots[i].temp2m;
    if (!any || t < lo) lo = t;
    if (!any || t > hi) hi = t;
    any = true;
  }
  return any;
}

// Run-length-encodes tonight's slots into at most maxSegs clear/poor
// segments. Stops growing past maxSegs rather than erroring — a night
// choppier than that just reads as "Variable overnight" (see
// tonightTrendPhrase()), so extra segments wouldn't be shown anyway.
int buildNightSegments(const NightWindow& nw, NightSegment* segs, int maxSegs) {
  int n = 0;
  bool haveCur = false, curClear = false;
  for (uint8_t i = 0; i < slotCount; i++) {
    if (!slotInWindow(i, nw)) continue;
    bool clear = calcScore(slots[i]) >= CLEAR_SCORE_CUTOFF;
    if (!haveCur || clear != curClear) {
      if (n >= maxSegs) break;
      segs[n].clear = clear;
      segs[n].start = lastFetchEpoch + (slots[i].timepoint * 3600L);
      n++;
      curClear = clear;
      haveCur = true;
    }
  }
  return n;
}

// Narrates how conditions change across tonight's window — a single average
// score can't distinguish "great all night" from "great early, ruined after
// midnight", so this describes the shape instead, across two lines so it can
// read as a full sentence rather than a clipped fragment. Patterns beyond a
// single dip/recovery collapse to a generic "Variable overnight" rather than
// trying to spell out every transition on a small screen.
void tonightTrendPhrase(char* line1, size_t l1size, char* line2, size_t l2size) {
  line2[0] = '\0';
  NightWindow nw;
  if (!dataValid || slotCount == 0 || !computeTonightWindow(nw)) {
    snprintf(line1, l1size, "No data for tonight");
    return;
  }
  NightSegment segs[4];
  int n = buildNightSegments(nw, segs, 4);
  if (n <= 0) {
    snprintf(line1, l1size, "No data for tonight");
    return;
  }

  struct tm lt;
  char t1[6], t2[6];
  switch (n) {
    case 1:
      snprintf(line1, l1size, "%s", segs[0].clear ? "Clear for the duration" : "Poor all night");
      break;
    case 2:
      localtime_r(&segs[1].start, &lt);
      snprintf(t1, sizeof(t1), "%02d:%02d", lt.tm_hour, lt.tm_min);
      if (segs[0].clear) {
        snprintf(line1, l1size, "Clear until %s,", t1);
        snprintf(line2, l2size, "degrading after that");
      } else {
        snprintf(line1, l1size, "Poor until %s,", t1);
        snprintf(line2, l2size, "clearing after that");
      }
      break;
    case 3: {
      localtime_r(&segs[1].start, &lt);
      snprintf(t1, sizeof(t1), "%02d:%02d", lt.tm_hour, lt.tm_min);
      localtime_r(&segs[2].start, &lt);
      snprintf(t2, sizeof(t2), "%02d:%02d", lt.tm_hour, lt.tm_min);
      int dipHours = (int)((segs[2].start - segs[1].start) / 3600L);
      if (segs[0].clear) {
        snprintf(line1, l1size, "Clear, dips around %s", t1);
        snprintf(line2, l2size, "for %dh, then clears again", dipHours);
      } else {
        snprintf(line1, l1size, "Poor, clears around %s", t1);
        snprintf(line2, l2size, "for %dh, then poor again", dipHours);
      }
      break;
    }
    default:
      snprintf(line1, l1size, "Variable overnight");
      break;
  }
}

// Sets the onboard WS2812 to tonight's verdict color (same score, same bands
// as the TONITE screen), or off if there's no data yet to score. Called
// whenever fetch state changes (see doFetchAstro()) — the underlying score
// only changes when new data arrives, so there's no need to redo this every
// loop() iteration.
void updateStatusLed() {
  if (!dataValid || slotCount == 0) {
    rgbLed.setPixelColor(0, 0);   // off — nothing to show a verdict for yet
  } else {
    static const uint32_t tierRgb[] = {
      0x00FF00,  // perfect — green
      0x80FF00,  // good — lime
      0xFFFF00,  // ok — yellow
      0xFF8C00,  // warn — orange
      0xFF0000,  // bad — red
    };
    int score = tonightAverageScore();
    if (score < 0) score = calcScore(slots[0]);   // fallback: window unavailable (e.g. clock not synced yet)
    rgbLed.setPixelColor(0, tierRgb[scoreTier(score)]);
  }
  rgbLed.show();
}

// Best-scoring slot within tonight's real sunset->sunrise window.
// Returns index into slots[], or -1 if none (window unavailable, or no
// slots fall inside it).
int bestNightSlot() {
  if (!dataValid || slotCount == 0) return -1;
  NightWindow nw;
  if (!computeTonightWindow(nw)) return -1;
  int bestIdx = -1;
  int bestScore = -1;
  for (uint8_t i = 0; i < slotCount; i++) {
    if (!slotInWindow(i, nw)) continue;
    int score = calcScore(slots[i]);
    if (score > bestScore) {
      bestScore = score;
      bestIdx = i;
    }
  }
  return bestIdx;
}

// ---------------------------------------------------------------------------
// Moon — phase, illumination and next rise/set, all computed on-device from
// the clock + location. No API we call carries moon data, but (like the
// sunrise/sunset math above) it doesn't need one. Phase and illumination
// come from the Moon's age since a known new moon. Rise/set use Montenbruck
// & Pfleger's low-precision lunar position ("MiniMoon", ~1 arcmin), sampled
// hour by hour with the horizon crossings found by linear interpolation —
// accurate to a minute or two, which is all a glanceable display needs.
// ---------------------------------------------------------------------------

static double moonFrac(double x) { return x - floor(x); }

// Greenwich mean sidereal time (radians) for a UT Modified Julian Date.
static double gmstRad(double mjd) {
  double mjd0 = floor(mjd);
  double ut   = (mjd - mjd0) * 86400.0;              // seconds since 0h UT
  double t0   = (mjd0 - 51544.5) / 36525.0;
  double t    = (mjd  - 51544.5) / 36525.0;
  double g    = 24110.54841 + 8640184.812866 * t0 + 1.0027379093 * ut
              + (0.093104 - 6.2e-6 * t) * t * t;
  g = fmod(g, 86400.0);
  if (g < 0) g += 86400.0;
  return g * (TWO_PI / 86400.0);
}

// Moon apparent RA (hours) and Dec (degrees) for Julian centuries T since
// J2000 — Montenbruck & Pfleger "MiniMoon", largest periodic terms only.
static void miniMoon(double T, double& raHours, double& decDeg) {
  const double ARC    = 206264.806;   // arcsec per radian
  const double COSEPS = 0.91748;      // cos / sin of the J2000 mean obliquity
  const double SINEPS = 0.39778;

  double l0 = moonFrac(0.606433 + 1336.855225 * T);          // mean long. (rev)
  double l  = TWO_PI * moonFrac(0.374897 + 1325.552410 * T); // Moon mean anomaly
  double ls = TWO_PI * moonFrac(0.993133 +   99.997361 * T); // Sun mean anomaly
  double d  = TWO_PI * moonFrac(0.827361 + 1236.853086 * T); // elongation
  double f  = TWO_PI * moonFrac(0.259086 + 1342.227825 * T); // arg. of latitude

  double dLam = 22640.0 * sin(l)
              - 4586.0  * sin(l - 2.0 * d)
              + 2370.0  * sin(2.0 * d)
              + 769.0   * sin(2.0 * l)
              - 668.0   * sin(ls)
              - 412.0   * sin(2.0 * f)
              - 212.0   * sin(2.0 * l - 2.0 * d)
              - 206.0   * sin(l + ls - 2.0 * d)
              + 192.0   * sin(l + 2.0 * d)
              - 165.0   * sin(ls - 2.0 * d)
              + 148.0   * sin(l - ls)
              - 125.0   * sin(d)
              - 110.0   * sin(l + ls)
              - 55.0    * sin(2.0 * f - 2.0 * d);

  double s = f + (dLam + 412.0 * sin(2.0 * f) + 541.0 * sin(ls)) / ARC;
  double h = f - 2.0 * d;
  double n = -526.0 * sin(h)
           + 44.0 * sin(l + h)
           - 31.0 * sin(-l + h)
           - 23.0 * sin(ls + h)
           + 11.0 * sin(-ls + h)
           - 25.0 * sin(-2.0 * l + f)
           + 21.0 * sin(-l + f);

  double lambda = TWO_PI * moonFrac(l0 + dLam / 1296000.0);  // ecliptic long (rad)
  double beta   = (18520.0 * sin(s) + n) / ARC;              // ecliptic lat  (rad)

  double cb  = cos(beta);
  double x   = cb * cos(lambda);
  double vy  = cb * sin(lambda);
  double w   = sin(beta);
  double y   = COSEPS * vy - SINEPS * w;
  double z   = SINEPS * vy + COSEPS * w;
  double rho = sqrt(1.0 - z * z);

  decDeg  = RAD_TO_DEG * atan2(z, rho);
  raHours = (24.0 / PI) * atan2(y, x + rho);   // 2*atan2, folded to 0..24 h
  if (raHours < 0.0) raHours += 24.0;
}

// sin(Moon altitude) minus its value at the horizon (mean parallax minus
// refraction and semidiameter, about +8'). A sign change between two hourly
// samples brackets a rise (- to +) or a set (+ to -).
static double moonAltMinusHorizon(time_t utc, double latRad, double lonRad) {
  double mjd = (double)utc / 86400.0 + 40587.0;   // Unix epoch = MJD 40587
  double T   = (mjd - 51544.5) / 36525.0;
  double ra, dec;
  miniMoon(T, ra, dec);
  double tau  = gmstRad(mjd) + lonRad - ra * (PI / 12.0);   // local hour angle
  double decR = dec * DEG_TO_RAD;
  double alt  = sin(latRad) * sin(decR) + cos(latRad) * cos(decR) * cos(tau);
  return alt - 0.00233;   // sin(+0.1333 deg)
}

// Next moonrise and next moonset (UTC epochs) at/after fromEpoch, by scanning
// 25 hours ahead one hour at a time. Either can come back 0: at mid-latitudes
// the Moon skips a rise or a set roughly one day a month (it drifts ~50 min
// later each day), and for longer near the poles.
void nextMoonRiseSet(float lat, float lon, time_t fromEpoch, time_t& riseOut, time_t& setOut) {
  double latRad = lat * DEG_TO_RAD;
  double lonRad = lon * DEG_TO_RAD;
  riseOut = 0;
  setOut  = 0;
  double prev = moonAltMinusHorizon(fromEpoch, latRad, lonRad);
  for (int hr = 1; hr <= 25; hr++) {
    time_t t   = fromEpoch + (time_t)hr * 3600L;
    double cur = moonAltMinusHorizon(t, latRad, lonRad);
    if (prev < 0.0 && cur >= 0.0 && riseOut == 0) {
      double fr = prev / (prev - cur);
      riseOut = fromEpoch + (time_t)((hr - 1 + fr) * 3600.0);
    }
    if (prev >= 0.0 && cur < 0.0 && setOut == 0) {
      double fr = prev / (prev - cur);
      setOut = fromEpoch + (time_t)((hr - 1 + fr) * 3600.0);
    }
    if (riseOut != 0 && setOut != 0) break;
    prev = cur;
  }
}

// Moon age, illuminated fraction and phase name, from the time since a
// reference new moon (2000-01-06 18:14 UTC); one synodic month is 29.530589
// days. Illuminated fraction is the standard (1 - cos(phase angle)) / 2.
MoonData computeMoon(time_t utc) {
  const double SYNODIC = 29.530588853;
  const time_t REF_NEW = 947182440L;   // 2000-01-06 18:14:00 UTC

  double age = fmod((double)(utc - REF_NEW) / 86400.0, SYNODIC);
  if (age < 0) age += SYNODIC;

  MoonData m;
  m.age    = age;
  m.illum  = (1.0 - cos(TWO_PI * age / SYNODIC)) / 2.0;
  m.waxing = age < SYNODIC / 2.0;

  double p = age / SYNODIC;                 // 0..1 through the cycle
  const double E = 0.02;                    // +/- ~0.6 day around each exact phase
  if      (p < E || p >= 1.0 - E) m.phase = "New Moon";
  else if (p < 0.25 - E)          m.phase = "Waxing Crescent";
  else if (p < 0.25 + E)          m.phase = "First Quarter";
  else if (p < 0.50 - E)          m.phase = "Waxing Gibbous";
  else if (p < 0.50 + E)          m.phase = "Full Moon";
  else if (p < 0.75 - E)          m.phase = "Waning Gibbous";
  else if (p < 0.75 + E)          m.phase = "Last Quarter";
  else                            m.phase = "Waning Crescent";
  return m;
}

// ---------------------------------------------------------------------------
// Fetch from 7timer — with retry for malformed JSON
// ---------------------------------------------------------------------------
bool fetchAstro() {
  // Plain HTTP, not HTTPS — 7timer serves this endpoint over HTTP directly (no
  // redirect), and there's no benefit paying the TLS handshake/session cost
  // for a plaintext public API with no sensitive data.
  WiFiClient client;
  client.setTimeout(8000);  // bound the TCP socket itself
  HTTPClient http;
  http.setTimeout(8000);   // bound the HTTP-layer read too, so a stalled request always
                           // returns (and gets retried) instead of hanging forever
  http.useHTTP10(true);    // avoid chunked transfer-encoding so getString() always has a
                           // declared Content-Length to read against

  char url[160];
  snprintf(url, sizeof(url),
    "http://www.7timer.info/bin/astro.php?lon=%.1f&lat=%.1f&ac=0&unit=metric&output=json&tzshift=0",
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
    int declaredLen = http.getSize();
    http.end();

    if (payload.length() == 0) {
      Serial.printf("[ASTRO] Empty body on attempt %d (Content-Length declared: %d)\n",
                    attempt, declaredLen);
      delay(2000);
      continue;
    }

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

// Mirrors the fetch cadence loop() schedules on: fast retry until the first
// success, then a fixed interval afterwards. Shared with screenSystem() so
// the on-screen countdown always matches what will actually happen.
uint32_t currentRetryIntervalMs() {
  if (dataValid) return FETCH_INTERVAL_MS;
  if (fetchAttempts < 10) return 10000UL;
  return 60000UL;
}

// Wraps fetchAstro() with the attempt counter that drives retry cadence and
// on-screen messaging (see drawNoDataMessage() and the loop() refresh logic),
// plus the bookkeeping screenSystem() needs to show fetch health.
void doFetchAstro() {
  lastFetchOk = fetchAstro();
  if (lastFetchOk) {
    fetchAttempts = 0;
    lastFetchEpoch = time(nullptr);
  } else {
    fetchAttempts++;
  }
  lastFetch = millis();
  updateStatusLed();   // score (if any) just changed — refresh the verdict LED
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

void drawText(int x, int y, uint16_t color, const char* s) {
  canvas->setTextColor(color);
  canvas->setCursor(x, y);
  canvas->print(s);
}

int textWidth(const char* s) {
  int16_t x1, y1;
  uint16_t w, h;
  canvas->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return (int)w;
}

void drawRightAligned(int rightX, int y, uint16_t color, const char* s) {
  drawText(rightX - textWidth(s), y, color, s);
}

void drawCentered(int centerX, int y, uint16_t color, const char* s) {
  drawText(centerX - textWidth(s) / 2, y, color, s);
}

void drawHeader(const char* title) {
  canvas->setFont(FONT_HDR);
  char left[24];
  snprintf(left, sizeof(left), "%s %d/%d", title, screen + 1, NUM_SCREENS);
  drawText(4, 21, COL_HEADER, left);               // HEADER: title + screen index

  char t[10];
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  strftime(t, sizeof(t), "%H:%M:%S", &lt);
  drawRightAligned(DISP_W - 4, 21, COL_TEXT, t);   // HEADER: clock right-aligned

  canvas->drawFastHLine(0, 28, DISP_W, COL_DIM);   // HEADER: divider line
}

// Small bar rating, width proportional to val/maxVal, at (x,y) w*h pixels
void drawBar(int x, int y, int w, int h, int val, int maxVal, uint16_t color) {
  canvas->drawRect(x, y, w, h, COL_TEXT);
  int fill = map(constrain(val, 0, maxVal), 0, maxVal, 0, w - 2);
  if (fill > 0) canvas->fillRect(x + 1, y + 1, fill, h - 2, color);
}

// Vertical variant of drawBar() — fills bottom-up instead of left-to-right,
// used by the TONITE screen's side gauge (see screenTonite()).
void drawBarVertical(int x, int y, int w, int h, int val, int maxVal, uint16_t color) {
  canvas->drawRect(x, y, w, h, COL_TEXT);
  int fill = map(constrain(val, 0, maxVal), 0, maxVal, 0, h - 2);
  if (fill > 0) canvas->fillRect(x + 1, y + h - 1 - fill, w - 2, fill, color);
}

// Shown on any screen while dataValid is false. Message depends on how many
// fetch attempts have happened since the last success: a brief note while
// still in the fast-retry window (every 10s, up to 10 tries), then longer
// guidance once that's expired and retries have backed off to every 60s.
void drawNoDataMessage() {
  if (fetchAttempts < 10) {
    canvas->setFont(FONT_BOLD);
    drawCentered(DISP_W / 2, 96, COL_TEXT, "Refreshing data");
  } else {
    canvas->setFont(FONT_MD);
    drawCentered(DISP_W / 2, 84,  COL_TEXT, "Retrying...");
    drawCentered(DISP_W / 2, 112, COL_DIM,  "Try a power cycle");
    drawCentered(DISP_W / 2, 140, COL_DIM,  "if this persists");
  }
}

// Score → GO / MARGINAL / NO GO label
const char* goNoGo(int score) {
  if (score >= 81) return "PERFECT";
  if (score >= 61) return "GOOD";
  if (score >= 41) return "MARGINAL";
  if (score >= 21) return "DOUBTFUL";
  return "TERRIBLE";
}

// ---------------------------------------------------------------------------
// Screen 1: TONITE — overall score + best window
// ---------------------------------------------------------------------------
void screenTonite() {
  drawHeader("TONITE");

  // Location name + Bortle rating, top of the right-hand column
  canvas->setFont(FONT_MD);
  char loc[48];
  snprintf(loc, sizeof(loc), "%s (BORTLE %u)", locationName, homeBortle);
  drawText(70, 62, COL_DIM, loc);  // TONITE: resolved place name + Bortle rating, level with gauge top

  // Small moon-phase disc, top-right corner — a glanceable "how bright is the
  // sky tonight" cue. Full phase/illumination/rise-set detail is on the MOON
  // screen. Only needs the clock, so it's drawn before the dataValid gate.
  time_t moonNow = time(nullptr);
  if (moonNow > 1000000000L) {
    MoonData md = computeMoon(moonNow);
    // waxing != (homeLat < 0): mirror the lit limb for southern-hemisphere
    // observers, who see the Moon flipped relative to the usual diagram.
    drawMoonDisc(288, 83, 15, md.illum, md.waxing != (homeLat < 0));  // TONITE: moon-phase disc
  }

  if (!dataValid) {
    drawNoDataMessage();
    return;
  }

  // Headline score/verdict = average across tonight's real sunset->sunrise
  // window (see tonightAverageScore()), not just the nearest forecast slot —
  // falls back to the nearest slot only if the window itself is unavailable
  // (e.g. clock not synced yet).
  int curScore = tonightAverageScore();
  if (curScore < 0) curScore = (slotCount > 0) ? calcScore(slots[0]) : 0;
  const char* verdict = goNoGo(curScore);
  uint16_t vcol = scoreColor(curScore);

  // Score gauge — vertical, down the left edge, so the verdict/trend column
  // keeps as much width as possible (the verdict is the one thing on this
  // screen that most needs to stay big).
  canvas->setFont(FONT_MD);
  char pctStr[6];
  snprintf(pctStr, sizeof(pctStr), "%d%%", curScore);
  drawCentered(32, 48, vcol, pctStr);                       // TONITE: score percentage, above the gauge
  drawBarVertical(10, 56, 44, 110, curScore, 100, vcol);     // TONITE: score gauge 0-100, fills bottom-up

  // Big verdict — FONT_LG rather than FONT_XL: the gauge now takes the left
  // edge, so "MARGINAL"/"DOUBTFUL"/"TERRIBLE" (the widest verdicts) need to
  // fit the narrower right-hand column rather than the full screen width.
  canvas->setFont(FONT_LG);
  drawText(70, 96, vcol, verdict);                          // TONITE: verdict text, large

  // Trend — how conditions are expected to change through the night, since
  // the headline score alone can't distinguish "great all night" from
  // "great early, ruined after midnight" (see tonightTrendPhrase()). Two
  // lines, so it reads as a full sentence rather than a clipped fragment.
  canvas->setFont(FONT_MD);
  char trend1[40], trend2[40];
  tonightTrendPhrase(trend1, sizeof(trend1), trend2, sizeof(trend2));
  drawText(70, 122, COL_TEXT, trend1);                      // TONITE: overnight trend phrase, line 1
  if (trend2[0]) drawText(70, 140, COL_TEXT, trend2);       // TONITE: overnight trend phrase, line 2

  // Best tonight window
  int best = bestNightSlot();
  if (best >= 0) {
    time_t slotTime = lastFetchEpoch + (slots[best].timepoint * 3600L);
    struct tm lt;
    localtime_r(&slotTime, &lt);
    char l[40];
    snprintf(l, sizeof(l), "BEST %02d:00 %d%% %s",
             lt.tm_hour, calcScore(slots[best]), cloudText(slots[best].cloudcover));
    drawText(70, 164, COL_DIM, l);              // TONITE: best imaging window time/score/cloud desc
  } else {
    drawText(70, 164, COL_DIM, "No good window tonight");
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

  // Show up to 6 timepoints as vertical bar chart. Bars are shorter than
  // they used to be (barMaxH/barBotY) specifically to leave enough room
  // below for two full-size (FONT_MD) label lines per bar instead of the
  // old tiny sub-labels.
  const uint8_t BARS    = min((int)slotCount, 6);
  const int     barW    = 34;
  const int     gap     = 6;
  const int     barMaxH = 70;
  const int     barBotY = 122;             // bottom of the bars
  const int     startX  = 14;
  const int     axisX   = startX + BARS * (barW + gap);

  for (uint8_t i = 0; i < BARS; i++) {
    int x = startX + i * (barW + gap);
    int c = slots[i].cloudcover;
    int barH = map(c, 1, 9, 6, barMaxH);
    uint16_t col = scoreColor(cloudPct(c));

    canvas->drawRect(x, barBotY - barMaxH, barW, barMaxH, COL_DIM);          // CLOUDS: bar outline
    canvas->fillRect(x + 1, barBotY - barH + 1, barW - 2, barH - 2, col);    // CLOUDS: cloud fill

    // Hour label
    time_t slotTime = lastFetchEpoch + (slots[i].timepoint * 3600L);
    struct tm lt;
    localtime_r(&slotTime, &lt);
    char h[6];
    snprintf(h, sizeof(h), "%02dh", lt.tm_hour);
    canvas->setFont(FONT_MD);
    drawCentered(x + barW / 2, barBotY + 20, COL_TEXT, h);   // CLOUDS: hour label

    // Cloud cover rating
    char cld[6];
    snprintf(cld, sizeof(cld), "%d/9", c);
    drawCentered(x + barW / 2, barBotY + 42, COL_DIM, cld);  // CLOUDS: cloud cover "N/9"
  }

  // Y-axis
  canvas->drawFastVLine(axisX, barBotY - barMaxH, barMaxH, COL_DIM);   // CLOUDS: Y-axis line
  canvas->setFont(FONT_MD);
  drawText(axisX + 6, barBotY - barMaxH + 16, COL_BAD,  "OVC");  // CLOUDS: top label
  drawText(axisX + 6, barBotY,                COL_GOOD, "CLR");  // CLOUDS: bottom label
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

  AstroSlot& s = slots[0];

  // Seeing — label+description on the left, big rating digit on the right
  canvas->setFont(FONT_MD);
  char row[24];
  snprintf(row, sizeof(row), "SEE  %s", seeingText(s.seeing));
  drawText(10, 64, COL_TEXT, row);                          // SEEING: label + description
  uint16_t seeCol = scoreColor(seeingPct(s.seeing));
  canvas->setFont(FONT_LG);
  char sv[3];
  snprintf(sv, sizeof(sv), "%d", s.seeing);
  drawText(240, 70, seeCol, sv);                            // SEEING: current seeing value, large

  // Transparency
  canvas->setFont(FONT_MD);
  snprintf(row, sizeof(row), "TRANSPCY  %s", transText(s.transparency));
  drawText(10, 106, COL_TEXT, row);                         // SEEING: label + description
  uint16_t transCol = scoreColor(seeingPct(s.transparency));
  canvas->setFont(FONT_LG);
  char tv[3];
  snprintf(tv, sizeof(tv), "%d", s.transparency);
  drawText(240, 112, transCol, tv);                         // SEEING: current transparency value, large

  // Lifted index — dynamically positioned since the numeric part's width varies
  canvas->setFont(FONT_MD);
  char li[20];
  snprintf(li, sizeof(li), "STABILITY: %+d", s.liftedindex);
  drawText(10, 144, COL_TEXT, li);                          // SEEING: lifted index (stability)
  const char* stab = (s.liftedindex >= 2)  ? "STABLE" :
                     (s.liftedindex >= -2)  ? "NEUTRAL" : "UNSTABLE";
  uint16_t stabCol = (s.liftedindex >= 2) ? COL_GOOD : (s.liftedindex >= -2) ? COL_OK : COL_BAD;
  drawText(10 + textWidth(li) + 20, 144, stabCol, stab);    // SEEING: stability label

  // Mini seeing trend for next slots
  canvas->setFont(FONT_MD);
  drawText(10, 166, COL_DIM, "TREND:");                     // SEEING: trend label
  int tx = 80;
  for (uint8_t i = 0; i < min((int)slotCount, 5); i++) {
    char sVal[3];
    snprintf(sVal, sizeof(sVal), "%d", slots[i].seeing);
    drawText(tx, 166, scoreColor(seeingPct(slots[i].seeing)), sVal);  // SEEING: seeing trend values
    tx += 46;
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
  canvas->setFont(FONT_MD);
  char l[40];

  NightWindow nw;
  bool haveWindow = computeTonightWindow(nw);

  // Dusk/dawn — tonight's real dark-hours window (see computeTonightWindow()).
  // Named DUSK/DAWN rather than RISE/SET: RISE conventionally means sunrise,
  // so an evening time under a "RISE" label would read backwards.
  if (haveWindow) {
    struct tm dlt, alt;
    localtime_r(&nw.start, &dlt);
    localtime_r(&nw.end, &alt);
    snprintf(l, sizeof(l), "DUSK %02d:%02d   DAWN %02d:%02d",
             dlt.tm_hour, dlt.tm_min, alt.tm_hour, alt.tm_min);
    drawText(10, 50, COL_TEXT, l);              // CONDTNS: tonight's dusk/dawn times
  }

  // Temperature — current reading, plus tonight's low/high when available.
  int lo, hi;
  if (haveWindow && nightTempRange(nw, lo, hi)) {
    snprintf(l, sizeof(l), "TEMP  %d\xB0" "C (%d-%d\xB0 tonight)", s.temp2m, lo, hi);
  } else {
    snprintf(l, sizeof(l), "TEMP  %d\xB0" "C", s.temp2m);
  }
  drawText(10, 80, COL_TEXT, l);               // CONDTNS: temperature (+ overnight range)

  // Wind
  snprintf(l, sizeof(l), "WIND  %s %dkm/h", s.winddir, windKmh(s.windspd));
  drawText(10, 109, COL_TEXT, l);              // CONDTNS: wind direction + speed

  // Humidity — highlighted if high enough to risk dew forming on optics
  bool dewRisk = s.rh2m >= 85;
  snprintf(l, sizeof(l), "HUM   %d%%", s.rh2m);
  drawText(10, 138, dewRisk ? COL_WARN : COL_TEXT, l);   // CONDTNS: relative humidity

  // Precipitation
  bool raining = strcmp(s.prectype, "none") != 0;
  snprintf(l, sizeof(l), "RAIN  %s", raining ? s.prectype : "NONE");
  drawText(10, 166, raining ? COL_BAD : COL_TEXT, l);    // CONDTNS: precipitation type
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

  const uint8_t ROWS = min((int)slotCount, 3);
  canvas->setFont(FONT_MD);

  // Column headers
  drawText(10,  48, COL_DIM, "TIME");         // FORECAST: column header - time
  drawText(130, 48, COL_DIM, "CLD");          // FORECAST: column header - cloud
  drawText(180, 48, COL_DIM, "SEE");          // FORECAST: column header - seeing
  drawText(230, 48, COL_DIM, "TRN");          // FORECAST: column header - transparency
  drawText(270, 48, COL_DIM, "GO?");          // FORECAST: column header - go/no-go
  canvas->drawFastHLine(0, 54, DISP_W, COL_DIM); // FORECAST: header divider

  for (uint8_t i = 0; i < ROWS; i++) {
    int y = 78 + i * 32;                      // 32px row spacing

    time_t slotTime = lastFetchEpoch + (slots[i].timepoint * 3600L);
    struct tm lt;
    localtime_r(&slotTime, &lt);

    char row[8];
    snprintf(row, sizeof(row), "%02d:00", lt.tm_hour);
    drawText(10, y, COL_TEXT, row);            // FORECAST: slot time

    snprintf(row, sizeof(row), "%d", slots[i].cloudcover);
    drawText(135, y, COL_TEXT, row);           // FORECAST: cloud cover value

    snprintf(row, sizeof(row), "%d", slots[i].seeing);
    drawText(185, y, COL_TEXT, row);           // FORECAST: seeing value

    snprintf(row, sizeof(row), "%d", slots[i].transparency);
    drawText(235, y, COL_TEXT, row);           // FORECAST: transparency value

    int score = calcScore(slots[i]);
    bool raining = strcmp(slots[i].prectype, "none") != 0;
    const char* go = raining ? "RAIN" : (score >= 81 ? "GO!" : score >= 61 ? "GO" : score >= 41 ? "OK" : score >= 21 ? "DBT" : "NO");
    uint16_t goCol = raining ? COL_BAD : scoreColor(score);
    drawText(270, y, goCol, go);                // FORECAST: go/no-go label
  }
}

// ---------------------------------------------------------------------------
// Screen 6: MOON — phase, illuminated %, and the next moonrise/moonset.
// All computed on-device from the clock + location (see computeMoon() /
// nextMoonRiseSet()), so this screen stays useful even while the 7timer
// fetch is failing — it only needs NTP time to be valid.
// ---------------------------------------------------------------------------

// Filled disc with the illuminated fraction lit. The terminator sits at the
// disc half-width scaled by (1 - 2*illum): 0 at the quarters, +/-1 at new/
// full. Lit side is the right for a waxing Moon, the left for a waning one.
void drawMoonDisc(int cx, int cy, int r, double illum, bool waxing) {
  canvas->fillCircle(cx, cy, r, COL_MOONDARK);
  for (int dy = -r; dy <= r; dy++) {
    double xw = sqrt((double)(r * r - dy * dy));
    if (xw < 1.0) continue;
    int xlo, xhi;
    if (waxing) {
      xlo = (int)ceil((1.0 - 2.0 * illum) * xw);
      xhi = (int)xw;
    } else {
      xlo = -(int)xw;
      xhi = (int)floor((2.0 * illum - 1.0) * xw);
    }
    if (xhi >= xlo)
      canvas->drawFastHLine(cx + xlo, cy + dy, xhi - xlo + 1, COL_TEXT);
  }
  canvas->drawCircle(cx, cy, r, COL_DIM);
}

void screenMoon() {
  drawHeader("MOON");

  time_t now = time(nullptr);
  if (now < 1000000000L) {          // NTP not synced yet — nothing to compute from
    canvas->setFont(FONT_MD);
    drawCentered(DISP_W / 2, 100, COL_DIM, "Waiting for clock");
    return;
  }

  MoonData m = computeMoon(now);

  // Phase disc down the left side — lit limb mirrored below the equator, where
  // the Moon appears flipped relative to the conventional (northern) diagram.
  drawMoonDisc(58, 104, 40, m.illum, m.waxing != (homeLat < 0));   // MOON: phase disc

  // Phase name
  canvas->setFont(FONT_MD);
  drawText(116, 58, COL_TEXT, m.phase);                     // MOON: phase name

  // Illuminated fraction — big, and colored by how sky-friendly it is
  // (darker sky = better), matching the traffic-light scheme used elsewhere.
  char l[32];
  snprintf(l, sizeof(l), "%.0f%% lit", m.illum * 100.0);
  canvas->setFont(FONT_LG);
  drawText(116, 92, scoreColor(100 - (int)(m.illum * 100.0)), l);  // MOON: illuminated %

  // Age in days since the new moon
  canvas->setFont(FONT_MD);
  snprintf(l, sizeof(l), "%.1f days old", m.age);
  drawText(116, 114, COL_DIM, l);                           // MOON: moon age

  // Next moonrise / moonset from now
  time_t moonrise, moonset;
  nextMoonRiseSet(homeLat, homeLon, now, moonrise, moonset);
  char rStr[8] = "--:--", sStr[8] = "--:--";
  struct tm lt;
  if (moonrise) { localtime_r(&moonrise, &lt); snprintf(rStr, sizeof(rStr), "%02d:%02d", lt.tm_hour, lt.tm_min); }
  if (moonset)  { localtime_r(&moonset,  &lt); snprintf(sStr, sizeof(sStr), "%02d:%02d", lt.tm_hour, lt.tm_min); }
  snprintf(l, sizeof(l), "RISE %s   SET %s", rStr, sStr);
  drawText(116, 148, COL_TEXT, l);                          // MOON: next rise / set
}

// ---------------------------------------------------------------------------
// Screen 7: SYSTEM — uptime + data-fetch diagnostics
// ---------------------------------------------------------------------------
void screenSystem() {
  drawHeader("SYSTEM");

  canvas->setFont(FONT_MD);
  char l[40];

  // Uptime since boot (millis() — and so this — wraps every ~49.7 days)
  uint32_t upSec = millis() / 1000;
  uint32_t days  = upSec / 86400;
  uint32_t hh    = (upSec % 86400) / 3600;
  uint32_t mm    = (upSec % 3600) / 60;
  uint32_t ss    = upSec % 60;
  if (days > 0) {
    snprintf(l, sizeof(l), "UP  %lud %02lu:%02lu:%02lu", days, hh, mm, ss);
  } else {
    snprintf(l, sizeof(l), "UP  %02lu:%02lu:%02lu", hh, mm, ss);
  }
  drawText(10, 57, COL_TEXT, l);                // SYSTEM: uptime since boot

  // Last successful data update, and how long ago
  if (lastFetchEpoch == 0) {
    snprintf(l, sizeof(l), "UPD  never");
  } else {
    struct tm lt;
    localtime_r(&lastFetchEpoch, &lt);
    char tbuf[10];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &lt);
    uint32_t ageMin = (uint32_t)(time(nullptr) - lastFetchEpoch) / 60;
    if (ageMin < 60) {
      snprintf(l, sizeof(l), "UPD  %s %lum ago", tbuf, ageMin);
    } else {
      snprintf(l, sizeof(l), "UPD  %s %luh%02lum ago", tbuf, ageMin / 60, ageMin % 60);
    }
  }
  drawText(10, 91, COL_TEXT, l);                 // SYSTEM: last successful fetch time + age

  // Countdown to the next scheduled fetch attempt
  uint32_t interval = currentRetryIntervalMs();
  uint32_t elapsed  = millis() - lastFetch;
  uint32_t remainS  = (elapsed < interval) ? (interval - elapsed) / 1000 : 0;
  snprintf(l, sizeof(l), "NEXT in %lum%02lus", remainS / 60, remainS % 60);
  drawText(10, 125, COL_TEXT, l);                // SYSTEM: countdown to next fetch attempt

  // Result of the most recent fetch attempt
  if (lastFetchOk) {
    drawText(10, 159, COL_GOOD, "STATUS  OK");   // SYSTEM: last fetch attempt result
  } else {
    snprintf(l, sizeof(l), "STATUS  FAILED x%u", fetchAttempts);
    drawText(10, 159, COL_BAD, l);
  }
}

// ---------------------------------------------------------------------------
// WiFi/location setup portal. Runs at boot (forcePortal=false: only opens if
// there's no saved WiFi or it can't connect) or on demand from loop() when
// the BOOT button is pressed (forcePortal=true: always opens).
//
// GPIO9 must NOT be held low across an actual power-on/reset — the ESP32-C6's
// boot ROM samples it at that exact moment and, if low, enters the UART
// flash/download bootloader instead of running this sketch at all (the same
// reason the ESP8266 build required this for GPIO0). So the button is only
// ever read here, well after boot has already completed normally, via
// loop()'s continuous polling.
//
// If the user saves new settings, we show a confirmation and reboot the
// device ourselves (ESP.restart()) rather than asking them to power cycle —
// this guarantees NTP/timezone/geocode/forecast all re-initialize cleanly
// with the new values, and this function never returns in that case.
//
// If the submitted WiFi credentials fail to connect, WiFiManager's default
// behaviour is to silently keep retrying inside the same blocking call with
// no visible feedback on our screen — setBreakAfterConfig(true) makes it
// return to us after any submission (success or failure) instead, so we can
// show a clear "couldn't connect, reopening" message and let the user try
// again immediately rather than staring at a frozen "SETUP MODE" screen.
// ---------------------------------------------------------------------------
bool runWifiSetup(bool forcePortal) {
  char latStr[16], lonStr[16], dwellStr[8], bortleStr[4];
  snprintf(latStr, sizeof(latStr), "%.5f", homeLat);
  snprintf(lonStr, sizeof(lonStr), "%.5f", homeLon);
  snprintf(dwellStr, sizeof(dwellStr), "%lu", (unsigned long)(screenDwellMs / 1000));
  snprintf(bortleStr, sizeof(bortleStr), "%u", homeBortle);

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
  WiFiManagerParameter html_bortle_link(
    "<p style='margin:8px 0 2px'>Find your <b>Bortle scale rating</b> (1=darkest sky, 9=brightest/"
    "most light-polluted) at <a href='https://www.lightpollutionmap.net' target='_blank'>lightpollutionmap.net</a> "
    "— click your location on the map.<br>"
    "<small>(this link may not load once you're connected to this WiFi — if so, "
    "look it up beforehand, write it down, and come back to enter it below)</small></p>");
  WiFiManagerParameter custom_bortle("bortle", "Bortle scale (1-9)", bortleStr, sizeof(bortleStr) - 1);
  WiFiManagerParameter custom_dwell("dwell", "Screen rotation time (seconds)", dwellStr, sizeof(dwellStr) - 1);

  bool settingsSaved = false;
  WiFiManager wm;
  wm.addParameter(&html_latlon_link);
  wm.addParameter(&custom_lat);
  wm.addParameter(&custom_lon);
  wm.addParameter(&html_tz_link);
  wm.addParameter(&custom_tz);
  wm.addParameter(&html_bortle_link);
  wm.addParameter(&custom_bortle);
  wm.addParameter(&custom_dwell);
  // Dark theme for the captive portal web page (not the LCD). WiFiManager's
  // own setDarkMode(true)/"invert" class only recolors the body/links/h1 and
  // leaves the form and button containers white — a full-page CSS filter
  // invert (the technique the library's own README recommends) covers
  // everything instead.
  wm.setCustomHeadElement("<style>html{filter: invert(100%); -webkit-filter: invert(100%);}</style>");
  wm.setSaveConfigCallback([&settingsSaved]() { settingsSaved = true; });
  wm.setAPCallback([](WiFiManager*) {
    canvas->fillScreen(COL_BG);
    canvas->setFont(FONT_BOLD);
    drawCentered(DISP_W / 2, 40, COL_HEADER, "SETUP MODE");
    canvas->setFont(FONT_MD);
    drawCentered(DISP_W / 2, 76,  COL_TEXT, "Join WiFi:");
    drawCentered(DISP_W / 2, 104, COL_TEXT, "AstroMonitor-Setup");
    drawCentered(DISP_W / 2, 138, COL_DIM,  "Then open 192.168.4.1");
    canvas->flush();
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
    canvas->fillScreen(COL_BG);
    canvas->setFont(FONT_BOLD);
    drawCentered(DISP_W / 2, 40, COL_BAD, "WIFI FAILED");
    canvas->setFont(FONT_MD);
    drawCentered(DISP_W / 2, 76,  COL_TEXT, "Could not connect to");
    drawCentered(DISP_W / 2, 104, COL_TEXT, "that network.");
    drawCentered(DISP_W / 2, 138, COL_DIM,  "Reopening setup...");
    canvas->flush();
    delay(3000);
    forcePortal = true;   // make sure the retry reopens the portal
  }

  if (settingsSaved && connected) {
    homeLat = atof(custom_lat.getValue());
    homeLon = atof(custom_lon.getValue());
    strncpy(tzString, custom_tz.getValue(), sizeof(tzString) - 1);
    tzString[sizeof(tzString) - 1] = '\0';

    long bortleVal = atol(custom_bortle.getValue());
    if (bortleVal < 1) bortleVal = 1;
    if (bortleVal > 9) bortleVal = 9;
    homeBortle = (uint8_t)bortleVal;

    long dwellSec = atol(custom_dwell.getValue());
    if (dwellSec < 2) dwellSec = 2;       // keep rotation sane at either extreme
    if (dwellSec > 120) dwellSec = 120;
    screenDwellMs = (uint32_t)dwellSec * 1000UL;

    // Location changed — old resolved name no longer applies until re-geocoded
    strncpy(locationName, "Location Unknown", sizeof(locationName) - 1);
    locationName[sizeof(locationName) - 1] = '\0';
    saveSettings(homeLat, homeLon, homeBortle, tzString, locationName, screenDwellMs);

    canvas->fillScreen(COL_BG);
    canvas->setFont(FONT_BOLD);
    drawCentered(DISP_W / 2, 48, COL_GOOD, "SETTINGS SAVED");
    canvas->setFont(FONT_MD);
    drawCentered(DISP_W / 2, 88,  COL_TEXT, "Restarting to apply");
    drawCentered(DISP_W / 2, 116, COL_TEXT, "new settings...");
    canvas->flush();
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

  if (!canvas->begin()) {
    Serial.println("[LCD] canvas->begin() failed!");
  }
  ledcAttach(PIN_LCD_BL, 5000, 8);
  ledcWrite(PIN_LCD_BL, 128);   // ~50% duty — Waveshare's guidance is to keep
                                // this panel's backlight at 50% or lower to
                                // avoid heat buildup behind the screen

  rgbLed.begin();
  rgbLed.setBrightness(255);  // full brightness, matches factory demo — lower
                              // if you want a dimmer ambient glow instead
  rgbLed.show();              // starts off; updateStatusLed() lights it once
                               // the first fetch completes (see doFetchAstro())

  canvas->fillScreen(COL_BG);
  canvas->setFont(FONT_BOLD);
  drawCentered(DISP_W / 2, 68, COL_HEADER, "ASTRO MONITOR");
  canvas->setFont(FONT_MD);
  drawCentered(DISP_W / 2, 104, COL_TEXT, "Connecting WiFi");
  canvas->flush();

  loadSettings();

  // BOOT button (GPIO9) is only ever polled from loop() — see the note on
  // runWifiSetup() for why it must not be read/held during boot itself.
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);

  bool connected = runWifiSetup(false);

  if (!connected) {
    canvas->fillScreen(COL_BG);
    canvas->setFont(FONT_BOLD);
    drawCentered(DISP_W / 2, 92, COL_BAD, "WIFI FAILED");
    canvas->flush();
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

  canvas->fillScreen(COL_BG);
  canvas->setFont(FONT_BOLD);
  drawCentered(DISP_W / 2, 68, COL_HEADER, "ASTRO MONITOR");
  canvas->setFont(FONT_MD);
  drawCentered(DISP_W / 2, 104, COL_TEXT, "Fetching data...");
  canvas->flush();

  // Weather fetch runs before the (non-essential) geocode lookup below, so
  // the actual point of this device gets first crack at a clean network
  // state right after boot.
  doFetchAstro();
  lastScreenChange = millis();

  // Resolve a human-readable place name once per location change (or once
  // on first boot if it's never been resolved) — cached in settings.json
  // afterwards so this isn't called on every boot.
  if (strcmp(locationName, "Location Unknown") == 0) {
    Serial.println("[GEO] Resolving location name...");
    if (fetchLocationName(homeLat, homeLon)) {
      saveSettings(homeLat, homeLon, homeBortle, tzString, locationName, screenDwellMs);
    } else {
      Serial.println("[GEO] Lookup failed, using \"Location Unknown\"");
    }
  }
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
  // BOOT button (GPIO9) gestures:
  //   single click  -> jump straight to the TONITE screen
  //   double click  -> open the setup portal
  //   hold 5+ sec   -> factory reset (wipes WiFi credentials + settings.json)
  // See runWifiSetup() for why this is only ever checked here, in loop(),
  // rather than at boot.
  if (digitalRead(PIN_BOOT_BTN) == LOW) {
    canvas->fillScreen(COL_BG);
    canvas->setFont(FONT_MD);
    drawCentered(DISP_W / 2, 42, COL_TEXT, "Button held");
    drawCentered(DISP_W / 2, 78,  COL_TEXT, "Release: TONITE");
    drawCentered(DISP_W / 2, 108, COL_TEXT, "Double-click: SETUP");
    drawCentered(DISP_W / 2, 138, COL_WARN, "Hold 5s: FACTORY RESET");
    canvas->flush();

    uint32_t holdStart = millis();
    while (digitalRead(PIN_BOOT_BTN) == LOW && millis() - holdStart < 5000) {
      delay(50);
    }

    if (millis() - holdStart >= 5000) {
      Serial.println("[CFG] Factory reset requested — erasing WiFi + settings");
      canvas->fillScreen(COL_BG);
      canvas->setFont(FONT_BOLD);
      drawCentered(DISP_W / 2, 68, COL_BAD, "FACTORY RESET");
      canvas->setFont(FONT_MD);
      drawCentered(DISP_W / 2, 104, COL_TEXT, "Erasing + restarting");
      canvas->flush();

      WiFiManager wm;
      wm.resetSettings();                // erase saved WiFi credentials
      LittleFS.remove("/settings.json"); // erase saved location/timezone
      delay(1500);
      ESP.restart();
      // never reached
    } else {
      // Released before 5s — this is either a single click or the first half
      // of a double-click. Give it a short window to see if a second press
      // follows before committing to the single-click action; this is the
      // unavoidable cost of telling the two gestures apart on one button.
      const uint32_t DOUBLE_CLICK_WINDOW_MS = 400;
      uint32_t releaseTime = millis();
      bool secondPress = false;
      while (millis() - releaseTime < DOUBLE_CLICK_WINDOW_MS) {
        if (digitalRead(PIN_BOOT_BTN) == LOW) {
          secondPress = true;
          break;
        }
        delay(10);
      }

      if (secondPress) {
        // Debounce: wait out the second press's release so the outer loop()
        // doesn't immediately see it as low and re-trigger this whole block.
        while (digitalRead(PIN_BOOT_BTN) == LOW) delay(10);
        // Double-click — open the portal now. If the user saves,
        // runWifiSetup() restarts the device itself and never returns. If
        // they back out or it times out, we just fall through and resume
        // normal operation below.
        runWifiSetup(true);
      } else {
        // Single click — jump straight to TONITE and restart the rotation
        // timer fresh from here.
        screen = 0;
        lastScreenChange = millis();
      }
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
  uint32_t retryInterval = currentRetryIntervalMs();
  if (millis() - lastFetch >= retryInterval) {
    doFetchAstro();
  }

  // Safety net: if we've had good data before but fetches have been stuck
  // failing for a long time, do a clean restart rather than silently
  // displaying the same stale forecast indefinitely.
  if (dataValid && (time(nullptr) - lastFetchEpoch) > STALE_DATA_RESTART_SEC) {
    Serial.println("[ASTRO] Data stale for too long — restarting to recover");
    canvas->fillScreen(COL_BG);
    canvas->setFont(FONT_BOLD);
    drawCentered(DISP_W / 2, 68, COL_BAD, "DATA STUCK STALE");
    canvas->setFont(FONT_MD);
    drawCentered(DISP_W / 2, 104, COL_TEXT, "Restarting to recover...");
    canvas->flush();
    delay(1500);
    ESP.restart();
    // never reached
  }

  // Draw
  canvas->fillScreen(COL_BG);
  switch (screen) {
    case 0: screenTonite();     break;  // Overall go/no-go + best window
    case 1: screenClouds();     break;  // Cloud cover bar chart
    case 2: screenSeeing();     break;  // Seeing + transparency
    case 3: screenConditions(); break;  // Wind, humidity, temp, precip
    case 4: screenForecast();   break;  // 24h table
    case 5: screenMoon();       break;  // Moon phase, illumination + rise/set
    case 6: screenSystem();     break;  // Uptime + fetch diagnostics
  }
  canvas->flush();

  delay(100);
}
