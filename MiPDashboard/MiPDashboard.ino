/**
 * @file MiPDashboard.ino
 * @brief Web dashboard for the MiP Power Up - D1 mini library.
 *
 * Hosts a multi-tab web interface on the ESP8266 that lets you:
 *   - Configure WiFi SSID and password + view scanned access points
 *   - Control head (eye) LEDs, chest LED, and play sounds
 *   - View system information (library, software, hardware versions + UART speed)
 *   - View a world clock with selectable timezones (auto-detect from browser)
 *   - View weather (OpenWeatherMap) and drive MiP LEDs from conditions
 *
 * Requires:
 *   - MiP Power Up - D1 mini library
 *     https://github.com/Tiogaplanet/MPU_D1_mini_lib
 *   - ArduinoJson (Sketch → Include Library → Manage Libraries → "ArduinoJson")
 *
 * Open a browser to http://<mip-ip>/ after the board connects to WiFi.
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>
#include <time.h>
#include <MiP_Power_Up_-_D1_mini.h>
#include "secrets.h" // Contains SSID, password, and the OpenWeatherMap API key.

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
// const char* SECRET_SSID = "Kept in secrets.h";
// const char* SECRET_PASS = "Kept in secrets.h";
const char* HOSTNAME = "mip-dashboard";

// OpenWeatherMap (user-supplied key)
// const char* SECRET_OWM_API_KEY = "Kept in secrets.h";

// Default city for weather (can be changed on the Weather tab)
String weatherCity = "Camp Springs,US";

// Temperature display units (default Fahrenheit)
bool useFahrenheit = true;

// How often to refresh weather + update LEDs (ms)
const unsigned long WEATHER_INTERVAL_MS = 5UL * 60UL * 1000UL;  // 5 minutes

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
MiP mip;
ESP8266WebServer server(80);

bool mipConnected = false;
bool timeSynced = false;

// Cached version info
String libraryVersion;
String softwareVersionStr;
String hardwareVersionStr;
uint32_t uartBaud = 0;

// Weather state
struct WeatherData {
  bool valid = false;
  String cityName;
  String description;
  float tempC = 0;
  float humidity = 0;
  float windSpeed = 0;
  float rain1h = 0;
  int weatherId = 0;
  String icon;
  String lastError;
  unsigned long lastFetchMs = 0;
} weather;

// Eye animation state for rain (independent random segments)
unsigned long lastEyeAnimMs = 0;
unsigned long eyeAnimIntervalMs = 0;            // 0 = solid on (no rain)
bool eyeState[4] = { true, true, true, true };  // current on/off per segment

// Alarm clock state
bool alarmEnabled = false;
int alarmHour = 7;    // 0-23 local Eastern
int alarmMinute = 0;  // 0-59
bool alarmTriggeredToday = false;
int lastAlarmDay = -1;  // day-of-month when we last fired
bool alarmRinging = false;
unsigned long lastAlarmSoundMs = 0;
unsigned long lastAlarmFlashMs = 0;
bool alarmFlashOn = false;

// Manual Control-tab override (until Weather tab is visited)
bool manualLedOverride = false;
int lastHeadLed[4] = { 1, 1, 1, 1 };  // 0=Off 1=On 2=BlinkSlow 3=BlinkFast
uint8_t lastChestR = 0, lastChestG = 255, lastChestB = 0;
uint16_t lastChestOn = 0, lastChestOff = 0;
int lastSoundIdx = 0;
int lastSoundVol = 4;

// ---- Timezone types (must be before any functions that use them; Arduino prototype gen) ----
enum MipDstRule : uint8_t { MIP_DST_NONE = 0,
                            MIP_DST_US,
                            MIP_DST_EU,
                            MIP_DST_AU };

struct TimeZoneInfo {
  const char* name;      // display name
  const char* abbrStd;   // standard abbreviation
  const char* abbrDst;   // daylight abbreviation (may equal std)
  int16_t offsetMinStd;  // minutes east of UTC (standard)
  int16_t offsetMinDst;  // minutes east of UTC (daylight)
  MipDstRule dst;
};

// Representative world zones (offsets in minutes). DST rules:
//   MIP_DST_US  – 2nd Sunday March → 1st Sunday November
//   MIP_DST_EU  – last Sunday March → last Sunday October
//   MIP_DST_AU  – 1st Sunday October → 1st Sunday April (southern)
static const TimeZoneInfo TIMEZONES[] = {
  { "UTC", "UTC", "UTC", 0, 0, MIP_DST_NONE },
  { "Pacific (Los Angeles)", "PST", "PDT", -480, -420, MIP_DST_US },
  { "Mountain (Denver)", "MST", "MDT", -420, -360, MIP_DST_US },
  { "Central (Chicago)", "CST", "CDT", -360, -300, MIP_DST_US },
  { "Eastern (New York)", "EST", "EDT", -300, -240, MIP_DST_US },
  { "Alaska (Anchorage)", "AKST", "AKDT", -540, -480, MIP_DST_US },
  { "Hawaii", "HST", "HST", -600, -600, MIP_DST_NONE },
  { "Arizona (no DST)", "MST", "MST", -420, -420, MIP_DST_NONE },
  { "Newfoundland", "NST", "NDT", -210, -150, MIP_DST_US },
  { "Sao Paulo", "BRT", "BRT", -180, -180, MIP_DST_NONE },
  { "Buenos Aires", "ART", "ART", -180, -180, MIP_DST_NONE },
  { "London", "GMT", "BST", 0, 60, MIP_DST_EU },
  { "Paris / Berlin", "CET", "CEST", 60, 120, MIP_DST_EU },
  { "Athens / Helsinki", "EET", "EEST", 120, 180, MIP_DST_EU },
  { "Moscow", "MSK", "MSK", 180, 180, MIP_DST_NONE },
  { "Dubai", "GST", "GST", 240, 240, MIP_DST_NONE },
  { "India (New Delhi)", "IST", "IST", 330, 330, MIP_DST_NONE },
  { "Bangladesh", "BST", "BST", 360, 360, MIP_DST_NONE },
  { "Thailand (Bangkok)", "ICT", "ICT", 420, 420, MIP_DST_NONE },
  { "China (Beijing)", "CST", "CST", 480, 480, MIP_DST_NONE },
  { "Hong Kong / Singapore", "HKT", "HKT", 480, 480, MIP_DST_NONE },
  { "Japan (Tokyo)", "JST", "JST", 540, 540, MIP_DST_NONE },
  { "Korea (Seoul)", "KST", "KST", 540, 540, MIP_DST_NONE },
  { "Australia (Perth)", "AWST", "AWST", 480, 480, MIP_DST_NONE },
  { "Australia (Adelaide)", "ACST", "ACDT", 570, 630, MIP_DST_AU },
  { "Australia (Sydney)", "AEST", "AEDT", 600, 660, MIP_DST_AU },
  { "New Zealand (Auckland)", "NZST", "NZDT", 720, 780, MIP_DST_AU },
  { "Fiji", "FJT", "FJT", 720, 720, MIP_DST_NONE },
};
static const int TIMEZONE_COUNT = sizeof(TIMEZONES) / sizeof(TIMEZONES[0]);

// Selected zone index (default Eastern until browser detection runs)
int selectedTz = 4;  // Eastern (New York)
bool tzAutoDetected = false;
bool tzUserChosen = false;  // true after manual timezone selection


// ---------------------------------------------------------------------------
// HTML helpers (includes a simple MiP-style favicon)
// ---------------------------------------------------------------------------
String htmlHeader(const String& title) {
  // Tiny SVG favicon: stylized robot head with two eye LEDs (MiP-ish)
  const char* favicon =
    "data:image/svg+xml,%3Csvg xmlns=%22http://www.w3.org/2000/svg%22 viewBox=%220 0 32 32%22%3E%3Crect width=%2232%22 height=%2232%22 rx=%226%22 fill=%22%231e293b%22/%3E%3Crect x=%226%22 y=%228%22 width=%2220%22 height=%2216%22 rx=%223%22 fill=%22%23334155%22/%3E%3Ccircle cx=%2212%22 cy=%2215%22 r=%223%22 fill=%22%233b82f6%22/%3E%3Ccircle cx=%2220%22 cy=%2215%22 r=%223%22 fill=%22%233b82f6%22/%3E%3Crect x=%2211%22 y=%2221%22 width=%2210%22 height=%222%22 rx=%221%22 fill=%22%2394a3b8%22/%3E%3C/svg%3E";

  String h = F(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<link rel='icon' href='");
  h += favicon;
  h += F("' type='image/svg+xml'>"
         "<link rel='shortcut icon' href='");
  h += favicon;
  h += F("' type='image/svg+xml'>"
         "<title>");
  h += title;
  h += F("</title><style>"
         "body{font-family:system-ui,sans-serif;margin:0;background:#0f172a;color:#e2e8f0}"
         "header{background:#1e293b;padding:1rem 1.5rem;display:flex;align-items:center;gap:1rem;flex-wrap:wrap}"
         "header h1{margin:0;font-size:1.25rem}"
         "nav{display:flex;gap:0.5rem;flex-wrap:wrap}"
         "nav a{color:#94a3b8;text-decoration:none;padding:0.4rem 0.8rem;border-radius:6px}"
         "nav a.active,nav a:hover{background:#334155;color:#f8fafc}"
         "main{max-width:720px;margin:1.5rem auto;padding:0 1rem}"
         ".card{background:#1e293b;border-radius:12px;padding:1.25rem;margin-bottom:1rem}"
         "label{display:block;margin:0.75rem 0 0.25rem;font-size:0.9rem;color:#94a3b8}"
         "input,select{width:100%;padding:0.5rem;border-radius:6px;border:1px solid #334155;"
         "background:#0f172a;color:#e2e8f0;box-sizing:border-box}"
         "button,.btn{display:inline-block;margin-top:1rem;padding:0.6rem 1.2rem;"
         "background:#3b82f6;color:white;border:none;border-radius:6px;cursor:pointer;font-size:1rem}"
         "button:hover,.btn:hover{background:#2563eb}"
         ".row{display:flex;gap:0.75rem;flex-wrap:wrap}"
         ".row > *{flex:1;min-width:120px}"
         ".msg{padding:0.75rem;border-radius:6px;margin-bottom:1rem}"
         ".ok{background:#14532d;color:#bbf7d0}"
         ".err{background:#7f1d1d;color:#fecaca}"
         "table{width:100%;border-collapse:collapse}"
         "td,th{padding:0.5rem;text-align:left;border-bottom:1px solid #334155}"
         "th{color:#94a3b8;font-weight:500}"
         ".clock{font-size:2.5rem;font-weight:600;text-align:center;margin:1rem 0}"
         ".clock-sub{text-align:center;color:#94a3b8;margin-bottom:1rem}"
         ".temp{font-size:2.2rem;font-weight:600}"
         "</style></head><body>"
         "<header><h1>MiP Dashboard</h1><nav>"
         "<a href='/'>Info</a>"
         "<a href='/control'>Control</a>"
         "<a href='/wifi'>WiFi</a>"
         "<a href='/clock'>Clock</a>"
         "<a href='/weather'>Weather</a>"
         "</nav></header><main>");
  return h;
}

String htmlFooter() {
  return F("</main></body></html>");
}

// ---------------------------------------------------------------------------
// Time helpers – selectable world timezones with DST
// ---------------------------------------------------------------------------

time_t timegm_compat(struct tm* tm) {
  int year = tm->tm_year + 1900;
  int mon = tm->tm_mon;
  if (mon > 11) {
    year += mon / 12;
    mon %= 12;
  } else if (mon < 0) {
    int years = (11 - mon) / 12;
    year -= years;
    mon += 12 * years;
  }

  int64_t days = 0;
  for (int y = 1970; y < year; y++) {
    days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
  }
  static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  for (int m = 0; m < mon; m++) {
    days += mdays[m];
    if (m == 1 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) days++;
  }
  days += tm->tm_mday - 1;
  return (time_t)(days * 86400LL + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

// Nth weekday of month (n=1..4 or 5=last). wday: 0=Sun..6=Sat. hourUtc approximate.
time_t nthWeekdayOfMonth(int year, int mon0, int n, int wday, int hourUtc) {
  struct tm m = {};
  m.tm_year = year - 1900;
  m.tm_mon = mon0;
  m.tm_mday = 1;
  m.tm_hour = 12;
  time_t t1 = timegm_compat(&m);
  gmtime_r(&t1, &m);
  int first = 1 + (wday - m.tm_wday + 7) % 7;

  int day;
  if (n >= 5) {
    // last weekday of month
    static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int dim = mdays[mon0];
    if (mon0 == 1 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) dim = 29;
    day = first;
    while (day + 7 <= dim) day += 7;
  } else {
    day = first + (n - 1) * 7;
  }

  m = {};
  m.tm_year = year - 1900;
  m.tm_mon = mon0;
  m.tm_mday = day;
  m.tm_hour = hourUtc;
  return timegm_compat(&m);
}

bool isDstActive(time_t utc, MipDstRule rule) {
  if (rule == MIP_DST_NONE) return false;
  struct tm tm;
  gmtime_r(&utc, &tm);
  int y = tm.tm_year + 1900;

  time_t start, end;
  if (rule == MIP_DST_US) {
    // 2nd Sunday March 07:00 UTC → 1st Sunday November 06:00 UTC (approx)
    start = nthWeekdayOfMonth(y, 2, 2, 0, 7);
    end = nthWeekdayOfMonth(y, 10, 1, 0, 6);
    return (utc >= start && utc < end);
  }
  if (rule == MIP_DST_EU) {
    // last Sunday March 01:00 UTC → last Sunday October 01:00 UTC
    start = nthWeekdayOfMonth(y, 2, 5, 0, 1);
    end = nthWeekdayOfMonth(y, 9, 5, 0, 1);
    return (utc >= start && utc < end);
  }
  if (rule == MIP_DST_AU) {
    // 1st Sunday October → 1st Sunday April (spans year boundary)
    time_t startThis = nthWeekdayOfMonth(y, 9, 1, 0, 16);  // Oct
    time_t endThis = nthWeekdayOfMonth(y, 3, 1, 0, 16);    // Apr
    // DST active from Oct → next Apr
    if (utc >= startThis) return true;  // Oct–Dec
    if (utc < endThis) return true;     // Jan–Apr
    // between Apr and Oct: standard
    return false;
  }
  return false;
}

int16_t zoneOffsetMin(time_t utc, int tzIndex) {
  if (tzIndex < 0 || tzIndex >= TIMEZONE_COUNT) tzIndex = 0;
  const TimeZoneInfo& z = TIMEZONES[tzIndex];
  return isDstActive(utc, z.dst) ? z.offsetMinDst : z.offsetMinStd;
}

const char* zoneAbbr(time_t utc, int tzIndex) {
  if (tzIndex < 0 || tzIndex >= TIMEZONE_COUNT) tzIndex = 0;
  const TimeZoneInfo& z = TIMEZONES[tzIndex];
  return isDstActive(utc, z.dst) ? z.abbrDst : z.abbrStd;
}

void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  timeSynced = false;
  for (int i = 0; i < 50; i++) {
    time_t now = time(nullptr);
    if (now > 1700000000) {
      timeSynced = true;
      break;
    }
    delay(100);
  }
}

String formatLocalTime(time_t utc) {
  int16_t off = zoneOffsetMin(utc, selectedTz);
  time_t local = utc + (off * 60L);
  struct tm tm;
  gmtime_r(&local, &tm);
  char buf[48];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d  %02d:%02d:%02d %s",
           tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec,
           zoneAbbr(utc, selectedTz));
  return String(buf);
}

String formatTimeForOffsetMin(int16_t offsetMin) {
  time_t utc = time(nullptr);
  time_t target = utc + (offsetMin * 60L);
  struct tm tm;
  gmtime_r(&target, &tm);
  char buf[16];
  strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
  return String(buf);
}

void getLocalHms(time_t utc, int& hour, int& minute, int& day) {
  int16_t off = zoneOffsetMin(utc, selectedTz);
  time_t local = utc + (off * 60L);
  struct tm tm;
  gmtime_r(&local, &tm);
  hour = tm.tm_hour;
  minute = tm.tm_min;
  day = tm.tm_mday;
}

// Pick best timezone matching browser offset (minutes east of UTC) and optional IANA name
int matchTimezone(int offsetMin, const String& ianaName) {
  time_t utc = time(nullptr);
  if (utc < 1700000000) utc = 1700000000;  // fallback epoch if not synced

  int best = -1;
  int bestScore = -1;
  String lowerName = ianaName;
  lowerName.toLowerCase();

  for (int i = 0; i < TIMEZONE_COUNT; i++) {
    int16_t eff = zoneOffsetMin(utc, i);
    if (eff != offsetMin) continue;

    int score = 10;  // offset match
    String zn = String(TIMEZONES[i].name);
    zn.toLowerCase();
    // boost if IANA name keywords appear in our label
    if (lowerName.length() > 0) {
      if (lowerName.indexOf("los_angeles") >= 0 && zn.indexOf("los angeles") >= 0) score += 50;
      if (lowerName.indexOf("new_york") >= 0 && zn.indexOf("new york") >= 0) score += 50;
      if (lowerName.indexOf("chicago") >= 0 && zn.indexOf("chicago") >= 0) score += 50;
      if (lowerName.indexOf("denver") >= 0 && zn.indexOf("denver") >= 0) score += 50;
      if (lowerName.indexOf("london") >= 0 && zn.indexOf("london") >= 0) score += 50;
      if (lowerName.indexOf("paris") >= 0 && zn.indexOf("paris") >= 0) score += 50;
      if (lowerName.indexOf("berlin") >= 0 && zn.indexOf("berlin") >= 0) score += 50;
      if (lowerName.indexOf("tokyo") >= 0 && zn.indexOf("tokyo") >= 0) score += 50;
      if (lowerName.indexOf("sydney") >= 0 && zn.indexOf("sydney") >= 0) score += 50;
      if (lowerName.indexOf("auckland") >= 0 && zn.indexOf("auckland") >= 0) score += 50;
      if (lowerName.indexOf("phoenix") >= 0 && zn.indexOf("arizona") >= 0) score += 50;
      if (lowerName.indexOf("honolulu") >= 0 && zn.indexOf("hawaii") >= 0) score += 50;
      if (lowerName.indexOf("anchorage") >= 0 && zn.indexOf("alaska") >= 0) score += 50;
      if (lowerName.indexOf("sao_paulo") >= 0 && zn.indexOf("sao paulo") >= 0) score += 50;
      if (lowerName.indexOf("dubai") >= 0 && zn.indexOf("dubai") >= 0) score += 50;
      if (lowerName.indexOf("kolkata") >= 0 && zn.indexOf("india") >= 0) score += 50;
      if (lowerName.indexOf("shanghai") >= 0 && zn.indexOf("china") >= 0) score += 50;
      if (lowerName.indexOf("singapore") >= 0 && zn.indexOf("singapore") >= 0) score += 50;
      if (lowerName.indexOf("seoul") >= 0 && zn.indexOf("korea") >= 0) score += 50;
      if (lowerName.indexOf("moscow") >= 0 && zn.indexOf("moscow") >= 0) score += 50;
    }
    // prefer zones that have DST when offset could be ambiguous
    if (TIMEZONES[i].dst != MIP_DST_NONE) score += 1;
    if (score > bestScore) {
      bestScore = score;
      best = i;
    }
  }
  return best;  // -1 if no offset match
}

void startAlarm() {
  if (!mipConnected) return;
  alarmRinging = true;
  lastAlarmSoundMs = 0;
  lastAlarmFlashMs = 0;
  alarmFlashOn = false;
  // Play wake-up sound immediately
  mip.sound.play(MIP_SOUND_MIP_HI_CONFIDENT, MIP_VOLUME_7);
  lastAlarmSoundMs = millis();
}

void stopAlarm() {
  alarmRinging = false;
  // Return eyes + chest to weather reporting
  applyWeatherToMip();
}

void serviceAlarm() {
  if (!timeSynced || !alarmEnabled) return;

  time_t utc = time(nullptr);
  int h, m, day;
  getLocalHms(utc, h, m, day);

  // Reset "triggered today" flag at midnight
  if (day != lastAlarmDay && lastAlarmDay >= 0) {
    alarmTriggeredToday = false;
  }

  // Fire alarm once when local time matches
  if (!alarmRinging && !alarmTriggeredToday && h == alarmHour && m == alarmMinute) {
    alarmTriggeredToday = true;
    lastAlarmDay = day;
    startAlarm();
  }

  if (!alarmRinging) return;

  // Stop when face-down or on-back
  if (mipConnected) {
    mip.position.read();
    if (mip.position.isFaceDown() || mip.position.isOnBack()) {
      stopAlarm();
      return;
    }
  }

  // Flash LEDs ~2 Hz
  if (millis() - lastAlarmFlashMs >= 250) {
    lastAlarmFlashMs = millis();
    alarmFlashOn = !alarmFlashOn;
    if (mipConnected) {
      MiPHeadLED eye = alarmFlashOn ? MIP_HEAD_LED_ON : MIP_HEAD_LED_OFF;
      mip.headLEDs.write(eye, eye, eye, eye);
      if (alarmFlashOn) {
        mip.chestLED.write(255, 0, 0);  // red
      } else {
        mip.chestLED.write(0, 0, 255);  // blue
      }
    }
  }

  // Re-play sound every 4 seconds while ringing
  if (millis() - lastAlarmSoundMs >= 4000) {
    lastAlarmSoundMs = millis();
    if (mipConnected) {
      mip.sound.play(MIP_SOUND_MIP_HI_CONFIDENT, MIP_VOLUME_7);
    }
  }
}


// ---------------------------------------------------------------------------
// Weather + MiP LED mapping
// ---------------------------------------------------------------------------

String formatTemp(float tempC) {
  if (useFahrenheit) {
    float f = tempC * 9.0f / 5.0f + 32.0f;
    return String(f, 1) + " °F";
  }
  return String(tempC, 1) + " °C";
}

void tempToRgb(float tempC, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (tempC <= 0) {
    r = 0;
    g = 40;
    b = 255;
  } else if (tempC < 15) {
    float t = tempC / 15.0f;
    r = 0;
    g = (uint8_t)(40 + t * 215);
    b = (uint8_t)(255 - t * 255);
  } else if (tempC < 25) {
    float t = (tempC - 15) / 10.0f;
    r = (uint8_t)(t * 255);
    g = 255;
    b = 0;
  } else if (tempC < 35) {
    float t = (tempC - 25) / 10.0f;
    r = 255;
    g = (uint8_t)(255 - t * 255);
    b = 0;
  } else {
    r = 255;
    g = 0;
    b = 0;
  }
}

// Map rain intensity → how often we randomly toggle one eye segment (ms).
// 0 = no rain → solid on. Smaller interval = more intense / busier flashing.
unsigned long rainToAnimInterval(float rain1h, int weatherId) {
  // OpenWeatherMap: group 5xx = rain; also treat measurable rain1h
  bool isRain = (weatherId >= 500 && weatherId < 600) || rain1h > 0.05f;
  if (!isRain) return 0;

  // Intensity tiers (mm/h roughly matches OWM guidance)
  // drizzle/mist  < 0.5   → slow, sparse
  // light         0.5-2.5 → moderate
  // moderate      2.5-7.5 → busy
  // heavy         >= 7.5  → frantic
  if (rain1h >= 7.5f) return 120;  // heavy / intense
  if (rain1h >= 2.5f) return 250;  // moderate
  if (rain1h >= 0.5f) return 450;  // light
  return 800;                      // mist / drizzle
}

void applyWeatherToMip() {
  if (!mipConnected || !weather.valid) return;
  if (alarmRinging) return;       // alarm owns LEDs
  if (manualLedOverride) return;  // Control tab owns LEDs until Weather visited

  uint8_t r, g, b;
  tempToRgb(weather.tempC, r, g, b);
  mip.chestLED.write(r, g, b);

  eyeAnimIntervalMs = rainToAnimInterval(weather.rain1h, weather.weatherId);
  if (eyeAnimIntervalMs == 0) {
    // Dry: all eyes solid on
    for (int i = 0; i < 4; i++) eyeState[i] = true;
    mip.headLEDs.write(MIP_HEAD_LED_ON, MIP_HEAD_LED_ON,
                       MIP_HEAD_LED_ON, MIP_HEAD_LED_ON);
  }
  // else: loop() will randomly animate segments
}

void writeEyeStates() {
  if (!mipConnected) return;
  // Protocol led1 = rightmost when facing MiP; led4 = leftmost.
  // eyeState[0] is UI-left → protocol led4, eyeState[3] is UI-right → protocol led1.
  mip.headLEDs.write(
    eyeState[3] ? MIP_HEAD_LED_ON : MIP_HEAD_LED_OFF,  // protocol led1 (right)
    eyeState[2] ? MIP_HEAD_LED_ON : MIP_HEAD_LED_OFF,
    eyeState[1] ? MIP_HEAD_LED_ON : MIP_HEAD_LED_OFF,
    eyeState[0] ? MIP_HEAD_LED_ON : MIP_HEAD_LED_OFF);  // protocol led4 (left)
}


// Minimal URL-encoder for city query strings (spaces, commas, etc.)
String urlEncode(const String& s) {
  String out;
  out.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else if (c == ' ') {
      out += "%20";
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    weather.lastError = "No WiFi";
    weather.valid = false;
    return false;
  }

  String url = "https://api.openweathermap.org/data/2.5/weather?q=";
  url += urlEncode(weatherCity);  // "New York,US" → "New%20York%2CUS"
  url += "&units=metric&appid=";
  url += SECRET_OWM_API_KEY;

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();

  HTTPClient https;
  if (!https.begin(*client, url)) {
    weather.lastError = "HTTPS begin failed";
    weather.valid = false;
    return false;
  }

  int code = https.GET();
  if (code != HTTP_CODE_OK) {
    weather.lastError = "HTTP " + String(code);
    https.end();
    weather.valid = false;
    return false;
  }

  String payload = https.getString();
  https.end();

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    weather.lastError = String("JSON: ") + err.c_str();
    weather.valid = false;
    return false;
  }

  weather.cityName = doc["name"] | weatherCity.c_str();
  weather.tempC = doc["main"]["temp"] | 0.0f;
  weather.humidity = doc["main"]["humidity"] | 0.0f;
  weather.windSpeed = doc["wind"]["speed"] | 0.0f;
  weather.description = doc["weather"][0]["description"] | "";
  weather.weatherId = doc["weather"][0]["id"] | 0;
  weather.icon = doc["weather"][0]["icon"] | "";
  weather.rain1h = doc["rain"]["1h"] | 0.0f;
  weather.valid = true;
  weather.lastError = "";
  weather.lastFetchMs = millis();

  applyWeatherToMip();
  return true;
}

// ---------------------------------------------------------------------------
// Page handlers
// ---------------------------------------------------------------------------
void handleRoot() {
  if (mipConnected) {
    libraryVersion = mip.version.readMPUString();
    uartBaud = mip.getBaudRate();

    MiPSoftwareVersion sw;
    mip.version.readSoftware(sw);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d.%u",
             sw.year, sw.month, sw.day, sw.uniqueVersion);
    softwareVersionStr = buf;

    MiPHardwareInfo hw;
    mip.version.readHardware(hw);
    snprintf(buf, sizeof(buf), "Voice chip %u / Hardware %u",
             hw.voiceChip, hw.hardware);
    hardwareVersionStr = buf;
  }

  String page = htmlHeader("MiP – System Info");
  page += F("<div class='card'><h2>System Information</h2>");

  if (!mipConnected) {
    page += F("<p class='msg err'>Not connected to MiP robot.</p>");
  }

  page += F("<table>");
  page += F("<tr><th>Library version</th><td>");
  page += libraryVersion.length() ? libraryVersion : String("-");
  page += F("</td></tr>");
  page += F("<tr><th>MiP software version</th><td>");
  page += softwareVersionStr.length() ? softwareVersionStr : String("-");
  page += F("</td></tr>");
  page += F("<tr><th>MiP hardware</th><td>");
  page += hardwareVersionStr.length() ? hardwareVersionStr : String("-");
  page += F("</td></tr>");
  page += F("<tr><th>UART speed</th><td>");
  if (uartBaud) {
    page += String(uartBaud) + F(" baud");
  } else {
    page += F("-");
  }
  page += F("</td></tr>");
  page += F("<tr><th>ESP IP address</th><td>");
  page += WiFi.localIP().toString();
  page += F("</td></tr>");
  page += F("<tr><th>Hostname</th><td>");
  page += HOSTNAME;
  page += F(".local</td></tr>");
  page += F("</table></div>");
  page += htmlFooter();
  server.send(200, "text/html", page);
}

void handleControl() {
  String page = htmlHeader("MiP – Control");
  page += F("<div class='card'><h2>Head (Eye) LEDs</h2>");
  if (manualLedOverride) {
    page += F("<p style='color:#94a3b8;font-size:0.85rem'>Manual control active — weather LEDs paused until you open the Weather tab.</p>");
  }
  page += F("<p style='color:#94a3b8;font-size:0.85rem'>LED 1 is leftmost, LED 4 rightmost when facing MiP.</p>");
  page += F("<form method='POST' action='/setHead'><div class='row'>");

  const char* labels[] = { "LED 1 (left)", "LED 2", "LED 3", "LED 4 (right)" };
  const char* optNames[] = { "Off", "On", "Blink Slow", "Blink Fast" };
  for (int i = 0; i < 4; i++) {
    page += F("<div><label>");
    page += labels[i];
    page += F("</label><select name='led");
    page += String(i + 1);
    page += F("'>");
    for (int v = 0; v < 4; v++) {
      page += F("<option value='");
      page += String(v);
      page += F("'");
      if (lastHeadLed[i] == v) page += F(" selected");
      page += F(">");
      page += optNames[v];
      page += F("</option>");
    }
    page += F("</select></div>");
  }
  page += F("</div><button type='submit'>Set Head LEDs</button></form></div>");

  page += F("<div class='card'><h2>Chest LED</h2>"
            "<form method='POST' action='/setChest'>"
            "<div class='row'>"
            "<div><label>Red (0-255)</label><input type='number' name='r' min='0' max='255' value='");
  page += String(lastChestR);
  page += F("'></div>"
            "<div><label>Green (0-255)</label><input type='number' name='g' min='0' max='255' value='");
  page += String(lastChestG);
  page += F("'></div>"
            "<div><label>Blue (0-255)</label><input type='number' name='b' min='0' max='255' value='");
  page += String(lastChestB);
  page += F("'></div>"
            "</div>"
            "<div class='row'>"
            "<div><label>On time (ms)</label><input type='number' name='on' min='0' max='1000' value='");
  page += String(lastChestOn);
  page += F("'></div>"
            "<div><label>Off time (ms)</label><input type='number' name='off' min='0' max='1000' value='");
  page += String(lastChestOff);
  page += F("'></div>"
            "</div>"
            "<button type='submit'>Set Chest LED</button></form></div>");

  page += F("<div class='card'><h2>Play Sound</h2>"
            "<form method='POST' action='/playSound'>"
            "<label>Sound</label><select name='sound'>");
  const char* soundNames[] = {
    "Hi (confident)", "Burping", "Drinking", "Eating",
    "Fart", "Low battery", "App", "Sad"
  };
  for (int i = 0; i < 8; i++) {
    page += F("<option value='");
    page += String(i);
    page += F("'");
    if (lastSoundIdx == i) page += F(" selected");
    page += F(">");
    page += soundNames[i];
    page += F("</option>");
  }
  page += F("</select>"
            "<label>Volume (1-7)</label>"
            "<input type='number' name='vol' min='1' max='7' value='");
  page += String(lastSoundVol);
  page += F("'>"
            "<button type='submit'>Play</button></form></div>");

  page += htmlFooter();
  server.send(200, "text/html", page);
}

void handleWifi() {
  String page = htmlHeader("MiP – WiFi");

  page += F("<div class='card'><h2>Current Connection</h2><table>"
            "<tr><th>SSID</th><td>");
  page += WiFi.SSID();
  page += F("</td></tr><tr><th>IP</th><td>");
  page += WiFi.localIP().toString();
  page += F("</td></tr><tr><th>RSSI</th><td>");
  page += String(WiFi.RSSI()) + F(" dBm</td></tr></table></div>");

  page += F("<div class='card'><h2>Change WiFi</h2>"
            "<p>Enter new credentials and click Save. The board will attempt to "
            "reconnect and then restart.</p>"
            "<form method='POST' action='/setWifi'>"
            "<label>SSID</label><input type='text' name='ssid' maxlength='32' required>"
            "<label>Password</label><input type='password' name='pass' maxlength='64'>"
            "<button type='submit'>Save &amp; Reconnect</button></form></div>");

  page += F("<div class='card'><h2>Nearby Access Points</h2>");
  int n = WiFi.scanNetworks(false, true);
  if (n <= 0) {
    page += F("<p>No networks found (or scan failed).</p>");
  } else {
    page += F("<table><tr><th>SSID</th><th>RSSI</th><th>Channel</th><th>Encryption</th></tr>");
    for (int i = 0; i < n; i++) {
      page += F("<tr><td>");
      page += WiFi.SSID(i).length() ? WiFi.SSID(i) : String("(hidden)");
      page += F("</td><td>");
      page += String(WiFi.RSSI(i)) + F(" dBm</td><td>");
      page += String(WiFi.channel(i)) + F("</td><td>");
      switch (WiFi.encryptionType(i)) {
        case ENC_TYPE_NONE: page += F("Open"); break;
        case ENC_TYPE_WEP: page += F("WEP"); break;
        case ENC_TYPE_TKIP: page += F("WPA"); break;
        case ENC_TYPE_CCMP: page += F("WPA2"); break;
        case ENC_TYPE_AUTO: page += F("Auto"); break;
        default: page += F("?"); break;
      }
      page += F("</td></tr>");
    }
    page += F("</table>");
  }
  page += F("<p style='margin-top:1rem'><a class='btn' href='/wifi'>Refresh scan</a></p></div>");

  page += htmlFooter();
  server.send(200, "text/html", page);
}

void handleClock() {
  String page = htmlHeader("MiP – World Clock");

  // ----- Local time (selected timezone) -----
  page += F("<div class='card'><h2>Local Time</h2>");
  if (!timeSynced) {
    page += F("<p class='msg err'>Time not yet synchronized with NTP.</p>");
  } else {
    time_t now = time(nullptr);
    page += F("<div class='clock'>");
    page += formatLocalTime(now);
    page += F("</div>");
    page += F("<div class='clock-sub'>");
    page += TIMEZONES[selectedTz].name;
    if (tzAutoDetected) page += F(" · auto-detected from browser");
    page += F("</div>");
  }
  page += F("</div>");

  // ----- Alarm -----
  page += F("<div class='card'><h2>Alarm Clock</h2>");
  if (alarmRinging) {
    page += F("<p class='msg ok'>ALARM RINGING — tip MiP face-down or on its back to dismiss</p>");
  } else if (alarmEnabled) {
    page += F("<p class='msg ok'>Alarm armed for ");
    char tbuf[8];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", alarmHour, alarmMinute);
    page += tbuf;
    page += F(" (");
    page += TIMEZONES[selectedTz].name;
    page += F(")</p>");
  } else {
    page += F("<p style='color:#94a3b8'>Alarm is off</p>");
  }

  page += F("<form method='POST' action='/setAlarm'>"
            "<div class='row'>"
            "<div><label>Hour (0-23)</label>"
            "<input type='number' name='hour' min='0' max='23' value='");
  page += String(alarmHour);
  page += F("'></div>"
            "<div><label>Minute (0-59)</label>"
            "<input type='number' name='minute' min='0' max='59' value='");
  page += String(alarmMinute);
  page += F("'></div></div>"
            "<label style='margin-top:1rem'><input type='checkbox' name='enabled' value='1'");
  if (alarmEnabled) page += F(" checked");
  page += F("> Enable alarm</label>"
            "<button type='submit'>Save Alarm</button></form>");

  if (alarmRinging) {
    page += F("<form method='POST' action='/stopAlarm' style='margin-top:0.5rem'>"
              "<button type='submit'>Dismiss Alarm</button></form>");
  }
  page += F("</div>");

  // ----- Timezone selector (replaces old World Times fixed table as primary control) -----
  page += F("<div class='card'><h2>Timezone</h2>"
            "<p style='color:#94a3b8;font-size:0.9rem'>Select any zone below. "
            "On first visit the page tries to match your browser's timezone automatically.</p>"
            "<form method='POST' action='/setTimezone'>"
            "<label>Timezone</label><select name='tz'>");
  for (int i = 0; i < TIMEZONE_COUNT; i++) {
    page += F("<option value='");
    page += String(i);
    page += F("'");
    if (i == selectedTz) page += F(" selected");
    page += F(">");
    page += TIMEZONES[i].name;
    page += F("</option>");
  }
  page += F("</select>"
            "<button type='submit'>Apply Timezone</button></form></div>");

  // ----- Quick world reference (effective offsets with DST) -----
  page += F("<div class='card'><h2>World Times</h2><table>"
            "<tr><th>Zone</th><th>Time</th></tr>");
  // Show a curated subset by index
  const int preview[] = { 0, 1, 3, 4, 11, 12, 21, 25, 26 };
  time_t nowUtc = time(nullptr);
  for (int k = 0; k < 9; k++) {
    int i = preview[k];
    page += F("<tr><td>");
    page += TIMEZONES[i].name;
    page += F("</td><td>");
    if (timeSynced) {
      page += formatTimeForOffsetMin(zoneOffsetMin(nowUtc, i));
      page += F(" ");
      page += zoneAbbr(nowUtc, i);
    } else {
      page += F("-");
    }
    page += F("</td></tr>");
  }
  page += F("</table></div>");

  page += F("<div class='card'><h2>Resync NTP</h2>"
            "<form method='POST' action='/resyncTime'>"
            "<button type='submit'>Resync now</button></form></div>");

  // Browser timezone auto-detect (runs once per browser session)
  page += F(
    "<script>"
    "(function(){"
    "try{"
    "if(sessionStorage.getItem('mipTzSent'))return;"
    "var off=-new Date().getTimezoneOffset();"
    "var name='';"
    "try{name=Intl.DateTimeFormat().resolvedOptions().timeZone||'';}catch(e){}"
    "fetch('/detectTimezone?offset='+off+'&name='+encodeURIComponent(name))"
    ".then(function(r){return r.text();})"
    ".then(function(t){sessionStorage.setItem('mipTzSent','1');"
    "if(t==='changed')location.reload();});"
    "}catch(e){}"
    "})();"
    "</script>");

  page += htmlFooter();
  server.send(200, "text/html", page);
}

void handleWeather() {
  // Visiting Weather tab ends manual Control override
  if (manualLedOverride) {
    manualLedOverride = false;
    // Force a fresh fetch + LED update
    weather.lastFetchMs = 0;
  }

  if (!weather.valid || (millis() - weather.lastFetchMs > WEATHER_INTERVAL_MS)) {
    fetchWeather();
  } else if (mipConnected && weather.valid) {
    applyWeatherToMip();  // re-apply in case we just cleared override
  }

  String page = htmlHeader("MiP – Weather");

  page += F("<div class='card'><h2>Current Weather</h2>");
  if (!weather.valid) {
    page += F("<p class='msg err'>");
    page += weather.lastError.length() ? weather.lastError : String("No data yet");
    page += F("</p>");
  } else {
    page += F("<p class='temp'>");
    page += formatTemp(weather.tempC);
    page += F("</p>");
    page += F("<p style='text-transform:capitalize;margin-top:0'>");
    page += weather.description;
    page += F("</p><table>");
    page += F("<tr><th>City</th><td>");
    page += weather.cityName;
    page += F("</td></tr>");
    page += F("<tr><th>Humidity</th><td>");
    page += String(weather.humidity, 0) + F(" %</td></tr>");
    page += F("<tr><th>Wind</th><td>");
    page += String(weather.windSpeed, 1) + F(" m/s</td></tr>");
    page += F("<tr><th>Rain (1 h)</th><td>");
    page += String(weather.rain1h, 2) + F(" mm</td></tr>");
    page += F("<tr><th>Condition ID</th><td>");
    page += String(weather.weatherId) + F("</td></tr>");
    page += F("</table>");

    page += F("<p style='margin-top:1rem;color:#94a3b8'>MiP reaction: ");
    if (eyeAnimIntervalMs == 0) {
      page += F("eyes solid ON (no rain), ");
    } else {
      page += F("eyes randomly flashing (~");
      page += String(eyeAnimIntervalMs);
      page += F(" ms interval, rain), ");
    }
    page += F("chest color from temperature.</p>");
  }
  page += F("</div>");

  page += F("<div class='card'><h2>Change City</h2>"
            "<form method='POST' action='/setCity'>"
            "<label>City (e.g. London,UK or Annapolis,US)</label>"
            "<input type='text' name='city' value='");
  page += weatherCity;
  page += F("' maxlength='64' required>"
            "<button type='submit'>Update &amp; Fetch</button></form></div>");

  page += F("<div class='card'><h2>Temperature Units</h2>"
            "<form method='POST' action='/setUnits'>"
            "<label><input type='radio' name='units' value='F'");
  if (useFahrenheit) page += F(" checked");
  page += F("> Fahrenheit (°F)</label>"
            "<label><input type='radio' name='units' value='C'");
  if (!useFahrenheit) page += F(" checked");
  page += F("> Celsius (°C)</label>"
            "<button type='submit'>Apply</button></form></div>");

  page += F("<p><a class='btn' href='/weather'>Refresh now</a></p>");

  page += htmlFooter();
  server.send(200, "text/html", page);
}

// ---------------------------------------------------------------------------
// Action handlers
// ---------------------------------------------------------------------------
MiPHeadLED toHeadLed(int v) {
  switch (v) {
    case 0: return MIP_HEAD_LED_OFF;
    case 1: return MIP_HEAD_LED_ON;
    case 2: return MIP_HEAD_LED_BLINK_SLOW;
    case 3: return MIP_HEAD_LED_BLINK_FAST;
    default: return MIP_HEAD_LED_ON;
  }
}

void handleSetHead() {
  if (!mipConnected) {
    server.send(200, "text/html",
                htmlHeader("Error") + F("<p class='msg err'>MiP not connected</p>") + htmlFooter());
    return;
  }
  lastHeadLed[0] = constrain(server.arg("led1").toInt(), 0, 3);
  lastHeadLed[1] = constrain(server.arg("led2").toInt(), 0, 3);
  lastHeadLed[2] = constrain(server.arg("led3").toInt(), 0, 3);
  lastHeadLed[3] = constrain(server.arg("led4").toInt(), 0, 3);

  manualLedOverride = true;
  eyeAnimIntervalMs = 0;  // pause weather eye animation

  // UI LED1 (left) → protocol led4; UI LED4 (right) → protocol led1
  mip.headLEDs.write(
    toHeadLed(lastHeadLed[3]), toHeadLed(lastHeadLed[2]),
    toHeadLed(lastHeadLed[1]), toHeadLed(lastHeadLed[0]));

  server.sendHeader("Location", "/control");
  server.send(303);
}

void handleSetChest() {
  if (!mipConnected) {
    server.send(200, "text/html",
                htmlHeader("Error") + F("<p class='msg err'>MiP not connected</p>") + htmlFooter());
    return;
  }
  lastChestR = constrain(server.arg("r").toInt(), 0, 255);
  lastChestG = constrain(server.arg("g").toInt(), 0, 255);
  lastChestB = constrain(server.arg("b").toInt(), 0, 255);
  lastChestOn = constrain(server.arg("on").toInt(), 0, 1000);
  lastChestOff = constrain(server.arg("off").toInt(), 0, 1000);

  manualLedOverride = true;

  if (lastChestOn == 0 && lastChestOff == 0) {
    mip.chestLED.write(lastChestR, lastChestG, lastChestB);
  } else {
    mip.chestLED.write(lastChestR, lastChestG, lastChestB, lastChestOn, lastChestOff);
  }

  server.sendHeader("Location", "/control");
  server.send(303);
}

void handlePlaySound() {
  if (!mipConnected) {
    server.send(200, "text/html",
                htmlHeader("Error") + F("<p class='msg err'>MiP not connected</p>") + htmlFooter());
    return;
  }

  int idx = constrain(server.arg("sound").toInt(), 0, 7);
  int vol = constrain(server.arg("vol").toInt(), 1, 7);
  lastSoundIdx = idx;
  lastSoundVol = vol;
  MiPVolume volume = static_cast<MiPVolume>(vol);

  MiPSoundIndex sound;
  switch (idx) {
    case 0: sound = MIP_SOUND_MIP_HI_CONFIDENT; break;
    case 1: sound = MIP_SOUND_ACTION_BURPING; break;
    case 2: sound = MIP_SOUND_ACTION_DRINKING; break;
    case 3: sound = MIP_SOUND_ACTION_EATING; break;
    case 4: sound = MIP_SOUND_ACTION_FARTING_SHORT; break;
    case 5: sound = MIP_SOUND_MIP_LOW_BATTERY; break;
    case 6: sound = MIP_SOUND_MIP_APP; break;
    case 7: sound = MIP_SOUND_MOOD_SAD; break;
    default: sound = MIP_SOUND_MIP_HI_CONFIDENT; break;
  }

  mip.sound.play(sound, volume);

  server.sendHeader("Location", "/control");
  server.send(303);
}

void handleSetWifi() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");

  String page = htmlHeader("Reconnecting");
  page += F("<div class='card'><p class='msg ok'>Credentials received. "
            "Attempting to connect to <b>");
  page += ssid;
  page += F("</b>…</p>"
            "<p>If the connection succeeds the board will reboot and you "
            "should find it on the new network.</p></div>");
  page += htmlFooter();
  server.send(200, "text/html", page);
  delay(500);

  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
  }
  ESP.restart();
}


void handleSetTimezone() {
  int tz = server.arg("tz").toInt();
  if (tz >= 0 && tz < TIMEZONE_COUNT) {
    selectedTz = tz;
    tzAutoDetected = false;
    tzUserChosen = true;  // do not auto-override again
    alarmTriggeredToday = false;
  }
  server.sendHeader("Location", "/clock");
  server.send(303);
}

void handleDetectTimezone() {
  int offset = server.arg("offset").toInt();  // minutes east of UTC
  String name = server.arg("name");
  int match = matchTimezone(offset, name);
  if (tzUserChosen) {
    server.send(200, "text/plain", "user");
    return;
  }
  if (match >= 0 && match != selectedTz) {
    selectedTz = match;
    tzAutoDetected = true;
    alarmTriggeredToday = false;
    server.send(200, "text/plain", "changed");
  } else if (match >= 0) {
    tzAutoDetected = true;
    server.send(200, "text/plain", "same");
  } else {
    server.send(200, "text/plain", "none");
  }
}

void handleResyncTime() {
  syncTime();
  server.sendHeader("Location", "/clock");
  server.send(303);
}


void handleSetUnits() {
  String u = server.arg("units");
  useFahrenheit = (u != "C");  // default / anything else → F
  server.sendHeader("Location", "/weather");
  server.send(303);
}

void handleSetCity() {
  String city = server.arg("city");
  city.trim();
  if (city.length() > 0) {
    weatherCity = city;
    fetchWeather();
  }
  server.sendHeader("Location", "/weather");
  server.send(303);
}

// ---------------------------------------------------------------------------
// Setup & loop
// ---------------------------------------------------------------------------

void handleSetAlarm() {
  alarmHour = constrain(server.arg("hour").toInt(), 0, 23);
  alarmMinute = constrain(server.arg("minute").toInt(), 0, 59);
  alarmEnabled = server.hasArg("enabled");
  alarmTriggeredToday = false;  // allow re-fire if set to current minute
  if (alarmRinging && !alarmEnabled) {
    stopAlarm();
  }
  server.sendHeader("Location", "/clock");
  server.send(303);
}

void handleStopAlarm() {
  stopAlarm();
  server.sendHeader("Location", "/clock");
  server.send(303);
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println(F("MiP Dashboard starting…"));

  mipConnected = mip.begin();
  if (!mipConnected) {
    Serial.println(F("Failed to connect to MiP robot."));
  } else {
    Serial.println(F("Connected to MiP."));
    libraryVersion = mip.version.readMPUString();
    uartBaud = mip.getBaudRate();
  }

  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.begin(SECRET_SSID, SECRET_PASS);

  Serial.print(F("Connecting to WiFi"));
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(400);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("IP address: "));
    Serial.println(WiFi.localIP());
    syncTime();
    fetchWeather();
  } else {
    Serial.println(F("WiFi connection failed – starting Access Point mode."));
    WiFi.mode(WIFI_AP);
    WiFi.softAP("MiP-Dashboard", "miprobot");
    Serial.print(F("AP IP: "));
    Serial.println(WiFi.softAPIP());
  }

  if (MDNS.begin(HOSTNAME)) {
    Serial.println(F("mDNS responder started"));
  }

  server.on("/", handleRoot);
  server.on("/control", handleControl);
  server.on("/wifi", handleWifi);
  server.on("/clock", handleClock);
  server.on("/weather", handleWeather);
  server.on("/setHead", HTTP_POST, handleSetHead);
  server.on("/setChest", HTTP_POST, handleSetChest);
  server.on("/playSound", HTTP_POST, handlePlaySound);
  server.on("/setWifi", HTTP_POST, handleSetWifi);
  server.on("/resyncTime", HTTP_POST, handleResyncTime);
  server.on("/setTimezone", HTTP_POST, handleSetTimezone);
  server.on("/detectTimezone", HTTP_GET, handleDetectTimezone);

  server.on("/setCity", HTTP_POST, handleSetCity);
  server.on("/setUnits", HTTP_POST, handleSetUnits);
  server.on("/setAlarm", HTTP_POST, handleSetAlarm);
  server.on("/stopAlarm", HTTP_POST, handleStopAlarm);



  // Favicon – browsers often request /favicon.ico explicitly
  server.on("/favicon.ico", HTTP_GET, []() {
    static const char* svg =

      "data:image/svg+xml,%3Csvg xmlns=%22http://www.w3.org/2000/svg%22 viewBox=%220 0 32 32%22%3E%3Crect width=%2232%22 height=%2232%22 rx=%226%22 fill=%22%231e293b%22/%3E%3Crect x=%226%22 y=%228%22 width=%2220%22 height=%2216%22 rx=%223%22 fill=%22%23334155%22/%3E%3Ccircle cx=%2212%22 cy=%2215%22 r=%223%22 fill=%22%233b82f6%22/%3E%3Ccircle cx=%2220%22 cy=%2215%22 r=%223%22 fill=%22%233b82f6%22/%3E%3Crect x=%2211%22 y=%2221%22 width=%2210%22 height=%222%22 rx=%221%22 fill=%22%2394a3b8%22/%3E%3C/svg%3E";
    server.sendHeader("Location", svg);
    server.sendHeader("Cache-Control", "public, max-age=86400");
    server.send(302, "text/plain", "");
  });

  server.begin();
  Serial.println(F("HTTP server started"));
}

void loop() {
  server.handleClient();
  MDNS.update();

  serviceAlarm();

  if (WiFi.status() == WL_CONNECTED && (millis() - weather.lastFetchMs > WEATHER_INTERVAL_MS)) {
    fetchWeather();
  }

  // Rain eye animation: randomly toggle individual segments
  // Skip while alarm is ringing (alarm owns the LEDs)
  if (mipConnected && !alarmRinging && !manualLedOverride && eyeAnimIntervalMs > 0) {
    if (millis() - lastEyeAnimMs >= eyeAnimIntervalMs) {
      lastEyeAnimMs = millis();
      // Pick a random segment and flip it
      int seg = random(0, 4);
      eyeState[seg] = !eyeState[seg];
      // Keep at least one eye on so it doesn't go fully dark
      if (!eyeState[0] && !eyeState[1] && !eyeState[2] && !eyeState[3]) {
        eyeState[seg] = true;
      }
      writeEyeStates();
    }
  }
}
