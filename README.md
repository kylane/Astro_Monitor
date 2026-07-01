# Astro Sky Conditions Monitor

A standalone ESP8266 device that fetches real-time astronomy weather forecasts and displays them on a 1.3" OLED screen. At a glance it tells you whether tonight is worth setting up your telescope.

![Astro Monitor](images/IMG1.jpg)

---

## Hardware

| Component | Details |
|-----------|---------|
| Microcontroller | ESP8266 (FREENOVE ESP8266 / NodeMCU ESP-12E or similar) |
| Display | 1.3" SH1106 I2C OLED, 128×64 pixels |

### Wiring

| OLED Pin | ESP8266 Pin |
|----------|-------------|
| GND | GND |
| VCC | 3V3 |
| SCL | GPIO5 (D1) |
| SDA | GPIO4 (D2) |

---

## Software Setup

### 1. Arduino IDE Board Settings

- **Board:** NodeMCU 1.0 (ESP-12E Module)
- **Upload Speed:** 115200
- **Flash Size:** 4MB

Add the ESP8266 board package URL if you haven't already:
```
https://arduino.esp8266.com/stable/package_esp8266com_index.json
```

### 2. Libraries

Install both from **Sketch → Include Library → Manage Libraries**:

| Library | Author | Version |
|---------|--------|---------|
| U8g2 | olikraus | latest |
| ArduinoJson | Benoit Blanchon | 7.x |

### 3. Configuration

Edit `config.h` before uploading:

```cpp
#define WIFI_SSID      "your_network_name"
#define WIFI_PASSWORD  "your_password"
#define HOME_LAT       -27.65973   // your latitude
#define HOME_LON       152.88028   // your longitude
#define TIMEZONE       "AEST-10"   // your POSIX timezone string
```

Find your coordinates at https://www.latlong.net

For other timezone strings, see:
https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv

---

## Data Source

All forecast data comes from **7Timer!** (https://www.7timer.info), a free public astronomy weather service based on NOAA/NCEP GFS numerical weather models. No API key is required. Data is fetched every 30 minutes and covers the next 72 hours in 3-hour slots.

---

## Screens

The device rotates through 5 screens, each displayed for 6 seconds.

---

### Screen 1 — TONITE

**Overall go/no-go assessment for right now.**

```
TONITE 1/5                 21:04:33
████████████████░░░░░░░░░░░░░░░░  ← score bar
         STILL A GO
      Conditions are good
CLD:5  SEE:6  TRN:4
BEST WINDOW: 23:00  SCR:82
FEW CLOUDS
```

**Score bar** — fills left to right from 0 to 100. The fuller it is, the better the conditions.

**Verdict** is one of five ratings:

| Verdict | Score | Meaning |
|---------|-------|---------|
| PERFECT | 85–100 | Exceptional night, ideal for imaging |
| STILL A GO | 65–84 | Good conditions, worth setting up |
| OK | 45–64 | Marginal but usable |
| DOUBTFUL | 25–44 | Poor conditions, probably not worth it |
| NO GO | 0–24 | Bad conditions, stay inside |

**Score calculation** — weighted average of four factors:

| Factor | Weight | How it's scored |
|--------|--------|----------------|
| Cloud cover | 50% | 7timer scale 1–9, inverted (1=clear=100pts, 9=overcast=0pts) |
| Seeing | 25% | 7timer scale 1–8, direct (1=terrible=0pts, 8=excellent=100pts) |
| Transparency | 15% | 7timer scale 1–8, direct (same as seeing) |
| Lifted index | 10% | Atmospheric stability. ≥0 = stable = 100pts; negative = unstable |
| Precipitation | — | Any rain/snow automatically returns score of 0 regardless of other factors |

**BEST WINDOW** shows the highest-scoring 3-hour slot between 20:00 and 05:00 tonight, with its score and cloud description.

---

### Screen 2 — CLOUDS

**Bar chart of cloud cover for the next 18 hours (6 × 3-hour slots).**

```
CLOUDS 2/5                 21:04:33
 ┌──┐┌──┐┌──┐┌──┐┌──┐┌──┐ │CLR
 │  ││▓▓││▓▓││▓▓││  ││  │ │
 │  ││▓▓││▓▓││▓▓││  ││  │ │
 └──┘└──┘└──┘└──┘└──┘└──┘ │OVC
 20h 23h 02h 05h 08h 11h
 5/9 9/9 9/9 9/9 2/9 1/9
```

- **Taller bar = more cloud cover** (bad for astronomy)
- **Shorter bar = less cloud** (good for astronomy)
- The **CLR/OVC** labels on the right mark the top (clear) and bottom (overcast) of the scale
- Each bar label shows the **hour** and **cloud cover rating out of 9**

Cloud cover scale:
- 1 = Clear
- 2–3 = Few clouds / Mostly clear
- 4–5 = Partly cloudy
- 6–7 = Mostly cloudy
- 8–9 = Overcast

---

### Screen 3 — SEEING

**Atmospheric seeing and transparency — how steady and clear the air is.**

```
SEEING 3/5                 21:04:33
6  /8  GOOD
5  /8  ABOVE AVG
LFT IDX: +6              STABLE
SEE: 6  6  5  4  4
```

**Seeing** measures atmospheric turbulence. Poor seeing causes stars to twinkle and blur, reducing sharpness in long-exposure images. Scale 1–8:

| Rating | Label |
|--------|-------|
| 1 | Terrible |
| 2 | Bad |
| 3 | Poor |
| 4 | Below average |
| 5 | Average |
| 6 | Good |
| 7 | Very good |
| 8 | Excellent |

**Transparency** measures how clear the atmosphere is — how much light is absorbed by haze, humidity and dust. Same 1–8 scale as seeing.

**Lifted Index (LFT IDX)** measures atmospheric stability:
- **Positive (+)** = stable air = steady seeing = better for planetary/lunar imaging
- **Zero** = neutral
- **Negative (−)** = unstable air = turbulent seeing = harder to get sharp images

**SEE trend** shows seeing values for the next 5 × 3-hour slots.

---

### Screen 4 — CONDTNS

**Current surface conditions relevant to observing.**

```
CONDTNS 4/5               21:04:33
TEMP  12°C
WIND  NE 7km/h
HUM   65%
PREC  NONE
```

- **TEMP** — air temperature at 2m height (°C)
- **WIND** — direction and approximate speed. High wind causes vibration in mounts and can shake the telescope during long exposures
- **HUM** — relative humidity %. High humidity (>85%) risks dew forming on optics and mirrors
- **PREC** — precipitation type: NONE, rain, snow, etc.

---

### Screen 5 — FORECAST

**3-slot compact forecast table showing the next 9 hours.**

```
FORECAST 5/5              21:04:33
─────────────────────────────────
TIME  CLD  SEE  TRN  GO?
21:00   5    6    4   OK
00:00   2    7    6   GO
03:00   1    7    7   GO!
```

Columns:
- **TIME** — local time for that 3-hour slot
- **CLD** — cloud cover (1=clear → 9=overcast)
- **SEE** — seeing quality (1=terrible → 8=excellent)
- **TRN** — transparency (1=terrible → 8=excellent)
- **GO?** — quick verdict:

| Label | Meaning |
|-------|---------|
| GO! | Score ≥ 85, exceptional |
| GO | Score ≥ 65, good |
| OK | Score ≥ 45, marginal |
| DBT | Score ≥ 25, doubtful |
| NO | Score < 25, no go |
| RAIN | Precipitation detected |

---

## Adjusting behaviour

All timing and location settings are in `config.h`:

| Setting | Default | Description |
|---------|---------|-------------|
| `SCREEN_DWELL_MS` | 6000 | Milliseconds each screen is shown |
| `FETCH_INTERVAL_MS` | 1800000 | How often data is re-fetched (30 min) |
| `FETCH_RETRIES` | 3 | Retries if 7timer returns malformed JSON |

---

## Notes

- 7Timer! data updates every ~6 hours on their server, so fetching more often than every 30 minutes won't give fresher data
- The service occasionally returns malformed JSON — the built-in retry logic handles this automatically
- Data covers 72 hours ahead in 3-hour slots
- This project uses HTTP**S** to connect to 7timer with certificate verification disabled (acceptable for a public weather API with no sensitive data)

---

## License

MIT — do whatever you like with it.
