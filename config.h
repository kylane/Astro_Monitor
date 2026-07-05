#pragma once

// =============================================================================
// Astro Sky Conditions Monitor — Configuration
// =============================================================================
// WiFi is no longer configured here. On first boot the device opens a setup
// portal ("AstroMonitor-Setup") where you pick your WiFi network and enter
// your location/timezone/rotation time — see README.md. Press the FLASH
// button (GPIO0) at any time while the device is running to reopen that
// portal later (hold 5+ seconds instead for a full factory reset).
//
// The values below are just the initial defaults shown in the setup portal
// (and the fallback if settings haven't been saved yet).
// =============================================================================

// ---------------------------------------------------------------------------
// Location
// Decimal degrees. Negative = South / West.
// Find yours at: https://www.latlong.net
// ---------------------------------------------------------------------------
#define HOME_LAT   -27.4698    // Get your location from https://www.latlong.net (default: Brisbane, QLD)
#define HOME_LON   153.0251

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
// SCREEN_DWELL_MS is just the initial default — it's editable from the setup
// portal ("Screen rotation time") afterwards, same as location/timezone.
// ---------------------------------------------------------------------------
#define SCREEN_DWELL_MS    10000            // milliseconds each screen is shown (default: 10s)
#define FETCH_INTERVAL_MS  (30UL*60*1000)  // how often to re-fetch from 7timer (30 min)
#define FETCH_RETRIES      3               // number of retries on malformed JSON
