#pragma once

// =============================================================================
// Astro Sky Conditions Monitor — Configuration
// Edit this file with your own settings before uploading.
// =============================================================================

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

// ---------------------------------------------------------------------------
// Location
// Decimal degrees. Negative = South / West.
// Find yours at: https://www.latlong.net
// ---------------------------------------------------------------------------
#define HOME_LAT   -27.65973    // e.g. -27.65973 for Augustine Heights, QLD
#define HOME_LON   152.88028    // e.g. 152.88028 for Augustine Heights, QLD

// ---------------------------------------------------------------------------
// Timezone (POSIX format)
// See: https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
// Examples:
//   Brisbane (no DST):   "AEST-10"
//   Sydney (with DST):   "AEST-10AEDT,M10.1.0,M4.1.0/3"
//   London:              "GMT0BST,M3.5.0/1,M10.5.0"
//   New York:            "EST5EDT,M3.2.0,M11.1.0"
// ---------------------------------------------------------------------------
#define TIMEZONE   "AEST-10"

// ---------------------------------------------------------------------------
// Display timing
// ---------------------------------------------------------------------------
#define SCREEN_DWELL_MS    6000             // milliseconds each screen is shown
#define FETCH_INTERVAL_MS  (30UL*60*1000)  // how often to re-fetch from 7timer (30 min)
#define FETCH_RETRIES      3               // number of retries on malformed JSON
