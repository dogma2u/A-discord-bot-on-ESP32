/*
 MiniMe Discord bot (ESP32-S3 + SSD1327). Command list: Discord !help.

 Sketch map (top to bottom):
 1) Config, pins, NTP / Pacific DST
 2) Gateway state + 8-user presence / 24h command counts
 3) OLED dashboard, sleep, overlays (U8g2 drawStr; y = font baseline)
 4) GPIO, servo, DS18B20, NeoPixel
 5) Discord REST, public HTTP APIs, command handler
 6) Touch wake (GPIO 4) + USB VBUS ADC (GPIO 1 divider)
 7) Gateway websocket, bot Online/Idle, scheduled posts, setup / loop

 OLED sleep dims then blanks the panel only; ESP32 and Wi-Fi stay up.
 Touch is polled in loop() (no interrupt). Bot status: online on activity,
 idle after 5 min quiet; touch does not set Online.
*/
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_NeoPixel.h>
#include <driver/ledc.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <time.h>
#include <string.h>
#include <esp_idf_version.h>
// ====== USER CONFIG ======
// Local-only secrets. GitHub sketch keeps placeholders. Do not commit real values.
const char* WIFI_SSID     = "ssid";
const char* WIFI_PASSWORD = "password";
const char* BOT_TOKEN     = "bot token";
const char* WEATHER_API_KEY = "WEATHER_API_KEY";
const char* NASA_API_KEY    = "NASA_API_KEY";
const char* DEEPSEEK_API_KEY = "DEEPSEEK_API_KEY";
#define BOT_GUILD_ID "GUILD_ID"  // startup member fetch
// ====== OWNER AND CHANNEL IDS ======
const char* OWNER_ID_STR        = "OWNER_ID_STR";  // GPIO / servo
const char* TARGET_CHANNEL_ID  = "TARGET_CHANNEL_ID";  // commands + auto posts
const char* TARGET_CHANNEL_ID1 = "TARGET_CHANNEL_ID1";  // second command channel
// Discord content max is 2000. !ask max_tokens / JSON buffer sized to fit one message.
const int DISCORD_CONTENT_MAX = 2000;
const int DEEPSEEK_MAX_TOKENS = 900;
const size_t DEEPSEEK_JSON_DOC = 12288;
// ====== GPIO CONFIG ======
// WeAct ESP32-S3-N16R8 defaults. Change here if wiring differs.
const int RGB_LED_PIN = 48;
const int PIN_SERVO   = 47;
const int PIN_SET1    = 6;
const int PIN_SET2    = 7;
const int PIN_DS18B20 = 10;
// I2C SSD1327 128x128 (GND / VCC / SCL / SDA on the module)
const int PIN_I2C_SDA = 8;
const int PIN_I2C_SCL = 9;
const int PIN_TOUCH   = 4; // TOUCH4 — wire pad here
const uint32_t TOUCH_THRESHOLD = 2000; // constant gap: trip = rolling idle avg + this
// USB 5V (VBUS) → 10k → PIN_USB_VBUS_ADC → 10k → GND. Do not feed 5V straight into the pin.
const int PIN_USB_VBUS_ADC = 1;
const uint32_t USB_VBUS_R_HI = 10000; // ohms, 5V side
const uint32_t USB_VBUS_R_LO = 10000; // ohms, GND side
const uint8_t TOUCH_AVG_N = 16;       // rolling idle window
const unsigned long USB_VBUS_READ_MS = 500;
// ====== TIME CONFIG (NTP) — US Pacific DST ======
// NTP is UTC; offset applied in updateLocalTime.
WiFiUDP ntpUDP;
const long PST_OFFSET_SEC = -28800; // UTC-8
const long PDT_OFFSET_SEC = -25200; // UTC-7
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);

int civilDayOfWeek(int year, int month, int day) { // 0 = Sunday
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int y = year;
  if (month < 3) y--;
  return (y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7;
}

int nthSundayOfMonth(int year, int month, int nth) {
  int dow1 = civilDayOfWeek(year, month, 1);
  int firstSunday = 1 + ((7 - dow1) % 7);
  return firstSunday + (nth - 1) * 7;
}

bool isPacificDaylightTime(unsigned long utcEpoch) {
  time_t t = (time_t)utcEpoch;
  struct tm tmUtc;
  gmtime_r(&t, &tmUtc);
  int year = tmUtc.tm_year + 1900;
  int month = tmUtc.tm_mon + 1;
  int day = tmUtc.tm_mday;
  int hour = tmUtc.tm_hour;
  if (month < 3 || month > 11) return false;
  if (month > 3 && month < 11) return true;
  if (month == 3) {
    int startDay = nthSundayOfMonth(year, 3, 2); // 2nd Sunday, 2:00am PST = 10:00 UTC
    if (day < startDay) return false;
    if (day > startDay) return true;
    return hour >= 10;
  }
  int endDay = nthSundayOfMonth(year, 11, 1); // 1st Sunday, 2:00am PDT = 09:00 UTC
  if (day < endDay) return true;
  if (day > endDay) return false;
  return hour < 9;
}

void updateLocalTime() {
  timeClient.setTimeOffset(0);
  timeClient.update();
  unsigned long utc = timeClient.getEpochTime();
  timeClient.setTimeOffset(isPacificDaylightTime(utc) ? PDT_OFFSET_SEC : PST_OFFSET_SEC);
}
// ====== SCHEDULED & INTERVAL TASKS ======
// 4-hour sysinfo and 06:00 / 12:00 / 18:00 Pacific indoor-temp posts go to TARGET_CHANNEL_ID.
const unsigned long SYSINFO_INTERVAL_MS = 14400000UL; // 4 Hours
unsigned long lastSysInfoMillis = 0;
int lastSentHour = -1;
// ====== DISCORD GATEWAY ======
// Websocket to gateway.discord.gg. gwDoc lives in PSRAM (256KB). Do not put small TLS buffers in PSRAM.
WebSocketsClient gatewayWS;
WiFiClientSecure httpsClient;
DynamicJsonDocument* gwDoc = nullptr;
const size_t GW_DOC_PSRAM = 262144;   // 256KB
const uint32_t BOARD_PSRAM_BYTES = 8UL * 1024UL * 1024UL; // this ESP32-S3 board
bool gatewayConnected     = false;
bool identified           = false;
int  heartbeatIntervalMs   = 0;
unsigned long lastHeartbeatMillis = 0;
int lastSeq               = 0;
// Bot's own Discord status (Gateway OP 3). Not the 8-user OLED rows.
unsigned long lastBotActivityMillis = 0;
const unsigned long BOT_PRESENCE_IDLE_MS = 300000UL; // 5 minutes quiet -> Idle
uint8_t botDiscordStatus = 0; // 0=unset (re-send after disconnect), 1=idle, 2=online
const uint32_t CPU_MHZ_ACTIVE = 240;
const uint32_t CPU_MHZ_OLED_OFF_BOT_IDLE = 80;
void noteBotActivity();
void updateBotPresenceIdle();
void sendBotPresence(const char* status, bool afk);
// ====== DISPLAY STATE ======
// EA_W128128 shifts the picture down: firmware y=0 is not glass top; y=119/127 can clip.
U8G2_SSD1327_WS_128X128_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
void noteDisplayActivity();
void drawDashboard();

float dashTempC = -999.0f;
float dashTempF = -999.0f;
int lastServoDeg = 45;

// ====== USER TRACKING (8 dashboard rows) ======
// REST member list at boot; Gateway presence + 24h command counts on OLED.
struct TrackedUser {
  String userId, userName;
  uint8_t status;  // 0 Off, 1 Idle, 2 On, 3 DND
  uint32_t useCount24h;
  bool active;
};
static const uint8_t MAX_TRACKED_USERS = 8;
TrackedUser trackedUsers[MAX_TRACKED_USERS];
String cachedGuildIds[3];
uint8_t cachedGuildCount = 0;
unsigned long usesWindowStartMillis = 0;
const unsigned long USES_WINDOW_MS = 86400000UL;  // 24h

uint8_t statusFromDiscord(const char* s) {
  if (!s) return 0;
  if (!strcmp(s, "online")) return 2;
  if (!strcmp(s, "idle")) return 1;
  if (!strcmp(s, "dnd")) return 3;
  return 0;  // offline / unknown
}
const char* statusToWord(uint8_t s) {
  static const char* w[] = {"Off", "Idle", "On", "DND"};
  return s < 4 ? w[s] : "Off";
}

void clearTrackedSlot(uint8_t i) {
  trackedUsers[i].active = false;
  trackedUsers[i].userId = "";
  trackedUsers[i].userName = "";
  trackedUsers[i].status = 0;
  trackedUsers[i].useCount24h = 0;
}

void fillTrackedSlot(uint8_t i, const String& userId, const String& userName) {
  trackedUsers[i].active = true;
  trackedUsers[i].userId = userId;
  trackedUsers[i].userName = userName;
  trackedUsers[i].status = 0;
  trackedUsers[i].useCount24h = 0;
}

void initTrackedUsers() {
  for (uint8_t i = 0; i < MAX_TRACKED_USERS; i++) clearTrackedSlot(i);
}

void resetUseWindowIfNeeded() {
  unsigned long now = millis();
  if (usesWindowStartMillis == 0) {
    usesWindowStartMillis = now;
    return;
  }
  if (now - usesWindowStartMillis >= USES_WINDOW_MS) {
    usesWindowStartMillis = now;
    for (uint8_t i = 0; i < MAX_TRACKED_USERS; i++) {
      trackedUsers[i].useCount24h = 0;
    }
  }
}

int findUserIndex(const String& userId) {
  if (userId.length() == 0) return -1;
  for (uint8_t i = 0; i < MAX_TRACKED_USERS; i++) {
    if (trackedUsers[i].active && trackedUsers[i].userId == userId) return (int)i;
  }
  return -1;
}

int findFreeTrackedSlot() {
  for (uint8_t i = 0; i < MAX_TRACKED_USERS; i++) {
    if (!trackedUsers[i].active) return (int)i;
  }
  return -1;
}

int addOrPickUserSlot(const String& userId, const String& userName) {
  int idx = findUserIndex(userId);
  if (idx >= 0) {
    if (userName.length()) trackedUsers[idx].userName = userName;
    return idx;
  }

  idx = findFreeTrackedSlot();
  if (idx >= 0) {
    fillTrackedSlot((uint8_t)idx, userId, userName);
    return idx;
  }

  // Full: overwrite lowest 24h-use slot.
  uint8_t worst = 0;
  for (uint8_t i = 1; i < MAX_TRACKED_USERS; i++) {
    if (trackedUsers[i].useCount24h < trackedUsers[worst].useCount24h) worst = i;
  }
  fillTrackedSlot(worst, userId, userName);
  return (int)worst;
}

void recordUserUse(const String& userId, const String& userName) {
  resetUseWindowIfNeeded();
  if (userId.length() == 0) return;
  int idx = findUserIndex(userId);
  if (idx < 0) {
    idx = addOrPickUserSlot(userId, userName);
  }
  if (idx < 0) return;
  if (userName.length()) trackedUsers[idx].userName = userName;
  trackedUsers[idx].status = 2;  // command use => On on OLED
  trackedUsers[idx].useCount24h++;
  noteDisplayActivity();
}

void applyPresencesArray(JsonArray presences) {
  if (presences.isNull()) return;
  for (JsonObject p : presences) {
    const char* uid = p["user"]["id"];
    if (!uid) continue;
    int idx = findUserIndex(String(uid));
    if (idx < 0) continue;
    const char* st = p["status"] | "offline";
    trackedUsers[idx].status = statusFromDiscord(st);
  }
}

void gwSendJson(JsonDocument& doc) {
  String payload;
  serializeJson(doc, payload);
  gatewayWS.sendTXT(payload);
}

void requestTrackedUserPresences() {
  if (cachedGuildCount == 0) return;
  bool any = false;
  for (uint8_t i = 0; i < MAX_TRACKED_USERS; i++) {
    if (trackedUsers[i].active && trackedUsers[i].userId.length()) {
      any = true;
      break;
    }
  }
  if (!any) return;

  for (uint8_t g = 0; g < cachedGuildCount; g++) {
    if (cachedGuildIds[g].length() < 16) continue;
    StaticJsonDocument<768> doc;
    doc["op"] = 8;
    JsonObject d = doc.createNestedObject("d");
    d["guild_id"] = cachedGuildIds[g];
    d["limit"] = 0;
    d["presences"] = true;
    JsonArray ids = d.createNestedArray("user_ids");
    for (uint8_t i = 0; i < MAX_TRACKED_USERS; i++) {
      if (trackedUsers[i].active && trackedUsers[i].userId.length()) {
        ids.add(trackedUsers[i].userId);
      }
    }
    gwSendJson(doc);
  }
}

String discordDisplayName(JsonVariantConst user) {
  String name = user["global_name"] | "";
  if (name.length() == 0) name = user["username"] | "";
  return name;
}

void handlePresenceUpdate(JsonObject d) {
  const char* uid = d["user"]["id"];
  const char* st  = d["status"] | "offline";
  if (!uid) return;

  String name = discordDisplayName(d["user"]);
  int idx = findUserIndex(String(uid));
  if (idx < 0) {
    idx = addOrPickUserSlot(String(uid), name);
    if (idx < 0) return;
  }
  if (name.length()) trackedUsers[idx].userName = name;
  trackedUsers[idx].status = statusFromDiscord(st);
}

// Transient text on OLED baselines y=119 / y=127 (!display / command status).
String transientLine1 = "";
String transientLine2 = "";
String transientLine3 = "";
unsigned long transientUntilMs = 0;

unsigned long lastDashMillis = 0;
const unsigned long DASH_REFRESH_MS = 2000;
unsigned long lastDisplayActivityMillis = 0;

// ====== TOUCH WAKE (GPIO 4) ======
// Compensated raw vs rolling idle avg + TOUCH_THRESHOLD.
unsigned long lastTouchWakeMillis = 0;
bool touchWasActive = false;
uint32_t touchIdleBuf[TOUCH_AVG_N];
uint8_t touchIdleBufCount = 0;
uint8_t touchIdleBufIdx = 0;
uint32_t touchIdleSum = 0;
uint32_t touchIdleAvg = 0;
uint32_t usbVbusRefMv = 0;
uint32_t usbVbusMvCached = 0;
uint32_t usbVbusCompLastMv = 0;
unsigned long usbVbusLastReadMs = 0;
bool displayAsleep = false;
const unsigned long DISPLAY_IDLE_MS = 60000UL; // 1 minute full brightness
const unsigned long DISPLAY_DIM_MS = 15000UL; // 15 seconds fade to off
const unsigned long TOUCH_DEBOUNCE_MS = 300;
const uint8_t DISPLAY_CONTRAST_FULL = 255;

// ====== TEMP SENSOR (DS18B20 on PIN_DS18B20) ======
OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);
// ====== NEOPIXEL (owner !led) ======
Adafruit_NeoPixel pixels(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

// ====== FORWARD DECLARATIONS ======
bool readTemperature(float &tempC, float &tempF);
bool readHttpBodyAfterHeaders(Client& client, bool chunked, int contentLength,
                              String& outBody, unsigned long deadlineMs);
bool discordIdLooksValid(const String& id);
bool httpsConnect(const char* host, uint32_t timeoutMs = 15000);
uint8_t httpsGetOpen(const char* host, const String& path, unsigned long headerTimeoutMs,
                     const char* userAgent = "MiniMeBot/1.0",
                     const char* extraHeaders = nullptr); // 0 ok, 1 connect, 2 timeout
uint8_t httpGetOpen(WiFiClient& client, const char* host, const String& path, unsigned long headerTimeoutMs);
bool httpsAwaitHeaders(unsigned long deadlineMs, bool pump, String& outStatus,
                       bool& chunked, int& contentLength);
void setHttpOpenError(String& outReport, uint8_t err, const char* label);
void boardMemTotals(uint32_t& memFree, uint32_t& memTotal);
void uptimeDhms(unsigned long& days, unsigned long& hours, unsigned long& minutes);
void pumpGateway();

// ====== DISPLAY UTILS ======

void noteDisplayActivity() {
  lastDisplayActivityMillis = millis();
  if (displayAsleep) {
    displayAsleep = false;
    u8g2.setPowerSave(0);
    lastDashMillis = 0;
  }
  u8g2.setContrast(DISPLAY_CONTRAST_FULL);
}

bool touchSampleValid(uint32_t raw) {
  return raw > 0 && raw < 150000;
}

uint32_t touchTripPoint() {
  return touchIdleAvg + TOUCH_THRESHOLD;
}

uint32_t readUsbVbusMilliVolts() {
  unsigned long now = millis();
  if (usbVbusLastReadMs != 0 && (now - usbVbusLastReadMs) < USB_VBUS_READ_MS) {
    return usbVbusMvCached;
  }
  usbVbusLastReadMs = now;
  uint32_t pinMv = analogReadMilliVolts((uint8_t)PIN_USB_VBUS_ADC);
  uint32_t inst = (uint32_t)((uint64_t)pinMv * (USB_VBUS_R_HI + USB_VBUS_R_LO) / USB_VBUS_R_LO);
  if (usbVbusMvCached == 0) usbVbusMvCached = inst;
  else usbVbusMvCached = (usbVbusMvCached * 7u + inst) / 8u;
  return usbVbusMvCached;
}

// Compensated idle samples are raw * ref / V. When V steps, rescale the window so trip stays put.
void rescaleTouchIdleForVbus(uint32_t oldV, uint32_t newV) {
  if (oldV == 0 || newV == 0 || touchIdleBufCount == 0) return;
  touchIdleSum = 0;
  for (uint8_t i = 0; i < touchIdleBufCount; i++) {
    touchIdleBuf[i] = (uint32_t)((uint64_t)touchIdleBuf[i] * oldV / newV);
    touchIdleSum += touchIdleBuf[i];
  }
  touchIdleAvg = touchIdleSum / touchIdleBufCount;
}

// Scale touch raw to boot-time USB voltage so VBUS sag/swell does not walk the gap.
uint32_t compensateTouchRaw(uint32_t raw) {
  uint32_t v = readUsbVbusMilliVolts();
  if (usbVbusRefMv < 1000 || v < 1000) return raw;
  if (usbVbusCompLastMv >= 1000 && usbVbusCompLastMv != v) {
    rescaleTouchIdleForVbus(usbVbusCompLastMv, v);
  }
  usbVbusCompLastMv = v;
  return (uint32_t)((uint64_t)raw * usbVbusRefMv / v);
}

void setupUsbVbusAdc() {
  analogSetPinAttenuation(PIN_USB_VBUS_ADC, ADC_11db);
  pinMode(PIN_USB_VBUS_ADC, INPUT);
  uint32_t acc = 0;
  for (int i = 0; i < 8; i++) {
    usbVbusLastReadMs = 0;
    acc += readUsbVbusMilliVolts();
    delay(10);
  }
  usbVbusRefMv = acc / 8;
  usbVbusCompLastMv = usbVbusMvCached;
}

void touchIdlePush(uint32_t sample) {
  if (touchIdleBufCount < TOUCH_AVG_N) {
    touchIdleBuf[touchIdleBufCount++] = sample;
    touchIdleSum += sample;
  } else {
    touchIdleSum -= touchIdleBuf[touchIdleBufIdx];
    touchIdleBuf[touchIdleBufIdx] = sample;
    touchIdleSum += sample;
    touchIdleBufIdx = (uint8_t)((touchIdleBufIdx + 1) % TOUCH_AVG_N);
  }
  if (touchIdleBufCount > 0) touchIdleAvg = touchIdleSum / touchIdleBufCount;
}

void calibrateTouchIdleAvg() {
  touchIdleBufCount = 0;
  touchIdleBufIdx = 0;
  touchIdleSum = 0;
  touchIdleAvg = 0;
  for (int i = 0; i < TOUCH_AVG_N; i++) {
    uint32_t raw = (uint32_t)touchRead(PIN_TOUCH);
    if (touchSampleValid(raw)) touchIdlePush(compensateTouchRaw(raw));
    delay(25);
  }
  if (touchIdleBufCount == 0) {
    uint32_t raw = (uint32_t)touchRead(PIN_TOUCH);
    touchIdlePush(compensateTouchRaw(raw));
  }
}

void updateTouchIdleAvg(uint32_t compensated) {
  if (!touchSampleValid(compensated)) return;
  if (compensated >= touchTripPoint()) return;
  touchIdlePush(compensated);
}

void configureTouchHardware() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
  touchSetTiming(0.5f, 100); // Arduino ESP32 3.x / IDF 5.5+
#else
  touchSetCycles(1, 100);
#endif
}

void setupTouch() {
  setupUsbVbusAdc();
  configureTouchHardware();
  calibrateTouchIdleAvg();
}

bool touchIsActive() {
  uint32_t raw = (uint32_t)touchRead(PIN_TOUCH);
  uint32_t compensated = compensateTouchRaw(raw);
  if (compensated >= touchTripPoint()) return true;
  updateTouchIdleAvg(compensated);
  return false;
}

// Rising edge only — stuck trip must not keep resetting the idle timer.
void pollTouchWake() {
  bool active = touchIsActive();
  if (!active) {
    touchWasActive = false;
    return;
  }
  unsigned long now = millis();
  if (now - lastTouchWakeMillis < TOUCH_DEBOUNCE_MS) return;
  if (touchWasActive) return;
  touchWasActive = true;
  lastTouchWakeMillis = now;
  noteDisplayActivity();
}

// Idle: full 1 min, dim 15 s, then blank. Clock/RSSI/heap ticks do not count as activity.
void updateDisplaySleep() {
  if (displayAsleep) return;
  unsigned long now = millis();
  if (lastDisplayActivityMillis == 0) {
    lastDisplayActivityMillis = now;
    return;
  }

  unsigned long idle = now - lastDisplayActivityMillis;
  if (idle < DISPLAY_IDLE_MS) return;

  if (idle < DISPLAY_IDLE_MS + DISPLAY_DIM_MS) {
    unsigned long dimElapsed = idle - DISPLAY_IDLE_MS;
    uint8_t contrast = (uint8_t)(DISPLAY_CONTRAST_FULL -
      (dimElapsed * (unsigned long)DISPLAY_CONTRAST_FULL) / DISPLAY_DIM_MS);
    u8g2.setContrast(contrast);
    return;
  }

  displayAsleep = true;
  u8g2.setPowerSave(1); // panel off until touch or a real event
}

// Overlay on y=119/127; do not paint here (stack + NTP unsafe inside Gateway).
void showTransient(const String& line1, const String& line2 = "", const String& line3 = "", unsigned long durationMs = 3000) {
  noteDisplayActivity();
  transientLine1 = line1;
  transientLine2 = line2;
  transientLine3 = line3;
  transientUntilMs = millis() + durationMs;
  lastDashMillis = 0;
}

// Frame at (25, baselineY-7) size 103x8; fill at (26, baselineY-6).
void drawDashBar(const char* label, uint8_t baselineY, int fillW) {
  u8g2.drawStr(0, baselineY, label);
  u8g2.drawFrame(25, baselineY - 7, 103, 8);
  if (fillW > 0) u8g2.drawBox(26, baselineY - 6, fillW, 6);
}

// 5x7 → 6px/char, 8px/row. drawStr y = baseline.
// y=7 header | 15 bot+date | 23 up/temp | 31 sig | 39 heap | 47 srv
// y=55..111 users | 119/127 transient
void drawDashboard() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);

  updateLocalTime();
  String t = timeClient.getFormattedTime();
  u8g2.drawStr(0, 7, "MiniMe");
  u8g2.drawStr(42, 7, gatewayConnected ? "GW:Good" : "GW:Bad");
  int timeX = 128 - u8g2.getStrWidth(t.c_str());
  if (timeX < 0) timeX = 0;
  u8g2.drawStr(timeX, 7, t.c_str());

  {
    static const char* const DOW_NAME[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char* const MON_NAME[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                           "Jul","Aug","Sep","Oct","Nov","Dec"};
    time_t localEpoch = (time_t)timeClient.getEpochTime();
    struct tm tmLocal;
    gmtime_r(&localEpoch, &tmLocal);
    char botBuf[12];
    snprintf(botBuf, sizeof(botBuf), "Bot:%-6s",
             (botDiscordStatus == 2) ? "Online" : "Idle");
    u8g2.drawStr(0, 15, botBuf);

    char dateBuf[20];
    snprintf(dateBuf, sizeof(dateBuf), "%s %s %2d %04d",
             DOW_NAME[tmLocal.tm_wday], MON_NAME[tmLocal.tm_mon],
             tmLocal.tm_mday, tmLocal.tm_year + 1900);
    // Fixed slot width so DOW column does not shift.
    const char* dateSlot = "Www Mmm 99 9999";
    int dateX = 128 - u8g2.getStrWidth(dateSlot);
    if (dateX < 0) dateX = 0;
    u8g2.drawStr(dateX, 15, dateBuf);
  }

  unsigned long d = 0, h = 0, m = 0;
  uptimeDhms(d, h, m);
  char upTempBuf[32];
  if (dashTempC > -998.0f) {
    snprintf(upTempBuf, sizeof(upTempBuf), "Up:%4lud%2luh%2lum T:%3.0fF/%3.0fC",
             d, h, m, dashTempF, dashTempC);
  } else {
    snprintf(upTempBuf, sizeof(upTempBuf), "Up:%4lud%2luh%2lum T:--Error--", d, h, m);
  }
  u8g2.drawStr(0, 23, upTempBuf);

  long rssi = WiFi.RSSI();
  uint32_t memFree = 0, memTotal = 0;
  boardMemTotals(memFree, memTotal);

  int sigBarW = 0;
  if (rssi >= -40) sigBarW = 79;
  else if (rssi <= -100) sigBarW = 0;
  else sigBarW = (int)((rssi + 100) * 79 / 60);
  drawDashBar("Sig:", 31, sigBarW);

  int heapBarW = 0;
  if (memTotal > 0) {
    heapBarW = (int)((memFree * 79UL) / memTotal);
    if (heapBarW < 0) heapBarW = 0;
    if (heapBarW > 79) heapBarW = 79;
  }
  drawDashBar("Heap:", 39, heapBarW);

  const int srvInnerW = 101;
  int srvBarW = (lastServoDeg * srvInnerW) / 90;
  if (srvBarW < 0) srvBarW = 0;
  if (srvBarW > srvInnerW) srvBarW = srvInnerW;
  drawDashBar("Srv:", 47, srvBarW);

  const int gapPx = 4;
  const int statusW = u8g2.getStrWidth("Idle");
  const int botReserveW = u8g2.getStrWidth("Bot:999");
  int nameMaxPx = 128 - gapPx - statusW - gapPx - botReserveW;
  if (nameMaxPx < 16) nameMaxPx = 16;

  const char* nameSrc[MAX_TRACKED_USERS];
  int longestNamePx = 0;
  for (uint8_t i = 0; i < MAX_TRACKED_USERS; i++) {
    if (!trackedUsers[i].active) {
      nameSrc[i] = "---";
    } else if (trackedUsers[i].userName.length()) {
      nameSrc[i] = trackedUsers[i].userName.c_str();
    } else {
      nameSrc[i] = trackedUsers[i].userId.c_str();
    }
    int w = u8g2.getStrWidth(nameSrc[i]);
    if (w > longestNamePx) longestNamePx = w;
  }
  if (longestNamePx > nameMaxPx) longestNamePx = nameMaxPx;
  int statusX = longestNamePx + gapPx;

  for (uint8_t row = 0; row < MAX_TRACKED_USERS; row++) {
    uint8_t y = 55 + (row * 8);
    char name[40];
    strncpy(name, nameSrc[row], sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    while (strlen(name) > 1 && u8g2.getStrWidth(name) > longestNamePx) {
      name[strlen(name) - 1] = '\0';
    }
    u8g2.drawStr(0, y, name);

    u8g2.drawStr(statusX, y,
                 statusToWord(trackedUsers[row].active ? trackedUsers[row].status : 0));

    uint32_t uses = trackedUsers[row].active ? trackedUsers[row].useCount24h : 0;
    char botBuf[12];
    snprintf(botBuf, sizeof(botBuf), "Bot:%lu", (unsigned long)uses);
    int botX = 128 - u8g2.getStrWidth(botBuf);
    if (botX < statusX + statusW + 2) botX = statusX + statusW + 2;
    u8g2.drawStr(botX, y, botBuf);
  }

  String row15 = "";
  String row16 = "";
  if (millis() < transientUntilMs) {
    row15 = transientLine1;
    row16 = transientLine2;
    if (transientLine3.length()) {
      if (row16.length()) row16 += " ";
      row16 += transientLine3;
    }
  }
  u8g2.drawStr(0, 119, row15.c_str());
  u8g2.drawStr(0, 127, row16.c_str());

  u8g2.sendBuffer();
}

void updateDisplay() {
  updateDisplaySleep();
  if (displayAsleep) return;
  unsigned long now = millis();
  if (transientUntilMs != 0 && now >= transientUntilMs) {
    transientUntilMs = 0;
    lastDashMillis = 0;
  }
  if (lastDashMillis == 0 || now - lastDashMillis >= DASH_REFRESH_MS) {
    lastDashMillis = now;
    float c, f;
    if (readTemperature(c, f)) {
      dashTempC = c;
      dashTempF = f;
    }
    drawDashboard();
  }
}

// ====== SERVO (LEDC 50Hz PWM, owner !servo 0-90) ======
void setupServo() {
  ledc_timer_config_t timer = {
    .speed_mode       = LEDC_LOW_SPEED_MODE,
    .duty_resolution  = LEDC_TIMER_14_BIT,
    .timer_num        = LEDC_TIMER_0,
    .freq_hz          = 50,
    .clk_cfg          = LEDC_AUTO_CLK
  };
  ledc_timer_config(&timer);
  ledc_channel_config_t channel = {
    .gpio_num         = PIN_SERVO,
    .speed_mode       = LEDC_LOW_SPEED_MODE,
    .channel          = LEDC_CHANNEL_0,
    .intr_type        = LEDC_INTR_DISABLE,
    .timer_sel        = LEDC_TIMER_0,
    .duty             = 0,
    .hpoint           = 0
  };
  ledc_channel_config(&channel);
}
void setServoAngle(int angleDeg) {
  if (angleDeg < 0) angleDeg = 0;
  if (angleDeg > 90) angleDeg = 90;
  lastServoDeg = angleDeg;
  int pulseUs = 500 + (1500 * angleDeg / 90);
  uint32_t max_duty = (1 << 14) - 1;
  uint32_t duty = (pulseUs * max_duty) / 20000;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
void setupPins() {
  pinMode(RGB_LED_PIN, OUTPUT);
  digitalWrite(RGB_LED_PIN, LOW);
  pixels.begin();
  pixels.show();
  pinMode(PIN_SET1, OUTPUT);
  pinMode(PIN_SET2, OUTPUT);
  digitalWrite(PIN_SET1, LOW);
  digitalWrite(PIN_SET2, LOW);
  pinMode(PIN_DS18B20, INPUT_PULLUP);
  setupServo();
  setServoAngle(45);
}

bool readTemperature(float &tempC, float &tempF) {
  pinMode(PIN_DS18B20, INPUT_PULLUP);
  sensors.requestTemperatures();
  float c = sensors.getTempCByIndex(0);
  if (c == DEVICE_DISCONNECTED_C) {
    return false;
  }
  tempC = c;
  tempF = c * 9.0f / 5.0f + 32.0f;
  return true;
}
// ====== DISCORD REST ======

void boardMemTotals(uint32_t& memFree, uint32_t& memTotal) {
  uint32_t psramSize = ESP.getPsramSize();
  uint32_t psramFree = ESP.getFreePsram();
  if (psramSize < BOARD_PSRAM_BYTES) psramSize = BOARD_PSRAM_BYTES;
  if (ESP.getPsramSize() == 0 || psramFree < 1) psramFree = BOARD_PSRAM_BYTES;
  memTotal = ESP.getHeapSize() + psramSize;
  memFree = ESP.getFreeHeap() + psramFree;
}

void uptimeDhms(unsigned long& days, unsigned long& hours, unsigned long& minutes) {
  unsigned long sec = millis() / 1000;
  days = sec / 86400;
  hours = (sec % 86400) / 3600;
  minutes = (sec % 3600) / 60;
  if (days > 9999) days = 9999;
}

String getSystemInfo() {
  long rssi = WiFi.RSSI();
  uint32_t freeHeap = 0, totalHeap = 0;
  boardMemTotals(freeHeap, totalHeap);
  unsigned long days = 0, hours = 0, minutes = 0;
  uptimeDhms(days, hours, minutes);
  String uptimeStr = String(days) + "d " + String(hours) + "h " + String(minutes) + "m";
  return "📊 **System Diagnostics:**\n"
         "• **Uptime:** " + uptimeStr + "\n"
         "• **Free Heap:** " + String((unsigned long)freeHeap) + " / " +
         String((unsigned long)totalHeap) + " bytes\n"
         "• **WiFi RSSI:** " + String(rssi) + " dBm\n"
         "• **Gateway Status:** " + String((gatewayConnected && identified) ? "Connected" : "Disconnected") + "\n"
         "• **USB VBUS:** " + String((float)readUsbVbusMilliVolts() / 1000.0f, 3) + " V\n"
         "• **Firmware:** https://github.com/dogma2u/A-discord-bot-on-ESP32";
}
bool httpsInUse = false; // blocks re-entrant REST while DeepSeek holds httpsClient
bool sendDiscordMessage(const String& channelId, const String& content, bool suppressEmbeds = false) {
  if (httpsInUse) return false;
  String post = content;
  if (post.length() > DISCORD_CONTENT_MAX) post = post.substring(0, DISCORD_CONTENT_MAX - 3) + "...";
  httpsInUse = true;
  if (!httpsConnect("discord.com")) {
    httpsInUse = false;
    return false;
  }
  String url = "/api/v10/channels/" + channelId + "/messages";
  StaticJsonDocument<4096> doc;
  doc["content"] = post;
  doc["tts"] = false;
  if (suppressEmbeds) doc["flags"] = 4; // SUPPRESS_EMBEDS: link stays a URL, no GitHub card
  if (post.length() > 0 && doc["content"].isNull()) {
    httpsClient.stop();
    httpsInUse = false;
    return false;
  }

  String body;
  serializeJson(doc, body);
  String request =
    "POST " + url + " HTTP/1.1\r\n"
    "Host: discord.com\r\n"
    "Authorization: Bot " + String(BOT_TOKEN) + "\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: " + String(body.length()) + "\r\n"
    "Connection: close\r\n\r\n" +
    body;
  httpsClient.print(request);
  unsigned long deadline = millis() + 5000UL;
  String statusLine;
  bool chunked = false;
  int contentLength = -1;
  if (!httpsAwaitHeaders(deadline, false, statusLine, chunked, contentLength)) {
    httpsInUse = false;
    return false;
  }
  httpsClient.stop();
  httpsInUse = false;
  int code = 0;
  int sp = statusLine.indexOf(' ');
  if (sp >= 0) code = statusLine.substring(sp + 1).toInt();
  return code >= 200 && code < 300;
}
void sendBotPresence(const char* status, bool afk) {
  if (!gatewayConnected || !identified) return;
  StaticJsonDocument<256> doc;
  doc["op"] = 3;
  JsonObject d = doc.createNestedObject("d");
  d["since"] = nullptr;
  d.createNestedArray("activities");
  d["status"] = status;
  d["afk"] = afk;
  gwSendJson(doc);
}

void noteBotActivity() {
  lastBotActivityMillis = millis();
  if (!gatewayConnected || !identified) return;
  if (botDiscordStatus == 2) return;
  botDiscordStatus = 2;
  sendBotPresence("online", false);
}

void updateBotPresenceIdle() {
  if (!gatewayConnected || !identified) return;
  if (lastBotActivityMillis == 0) {
    lastBotActivityMillis = millis();
    return;
  }
  if (botDiscordStatus == 1) return;
  if (millis() - lastBotActivityMillis < BOT_PRESENCE_IDLE_MS) return;
  botDiscordStatus = 1;
  sendBotPresence("idle", true);
}

// 80 MHz only when OLED off and Discord Idle. Wi-Fi needs >= 80 MHz.
void applyCpuForIdleState() {
  bool slow = displayAsleep && botDiscordStatus == 1 && identified;
  uint32_t want = slow ? CPU_MHZ_OLED_OFF_BOT_IDLE : CPU_MHZ_ACTIVE;
  if (getCpuFrequencyMhz() == want) return;
  setCpuFrequencyMhz(want);
}

void sendIdentify() {
  StaticJsonDocument<1024> doc;
  doc["op"] = 2;
  JsonObject d = doc.createNestedObject("d");
  d["token"] = BOT_TOKEN;
  JsonObject props = d.createNestedObject("properties");
  props["os"]     = "linux";
  props["browser"] = "esp32";
  props["device"]  = "esp32";
  d["compress"]         = false;
  d["large_threshold"] = 250;
  d["intents"] = 37635; // GUILDS + MEMBERS + PRESENCES + MESSAGES + DMs + MESSAGE_CONTENT
  JsonObject presence = d.createNestedObject("presence");
  presence["since"] = nullptr;
  presence.createNestedArray("activities");
  presence["status"] = "online";
  presence["afk"] = false;
  gwSendJson(doc);
  lastBotActivityMillis = millis();
  botDiscordStatus = 2;
}

void sendHeartbeat() {
  StaticJsonDocument<256> doc;
  doc["op"] = 1;
  if (lastSeq == 0) {
    doc["d"] = nullptr;
  } else {
    doc["d"] = lastSeq;
  }
  gwSendJson(doc);
}
void pumpGateway() {
  gatewayWS.loop();
  if (heartbeatIntervalMs > 0 && gatewayConnected && identified) {
    unsigned long now = millis();
    if (now - lastHeartbeatMillis >= (unsigned long)heartbeatIntervalMs) {
      lastHeartbeatMillis = now;
      sendHeartbeat();
    }
  }
}
bool isLedByteToken(const String& s) {
  if (s.length() == 0 || s.length() > 3) return false;
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c < '0' || c > '9') return false;
  }
  int v = s.toInt();
  return v >= 0 && v <= 255;
}

void setLedRgb(uint8_t r, uint8_t g, uint8_t b) {
  pixels.setPixelColor(0, pixels.Color(r, g, b));
  pixels.show();
}

bool parseRgbTriplet(const String& args, uint8_t& r, uint8_t& g, uint8_t& b) {
  String a = args;
  a.trim();
  int sp1 = a.indexOf(' ');
  int sp2 = (sp1 >= 0) ? a.indexOf(' ', sp1 + 1) : -1;
  if (sp1 <= 0 || sp2 <= sp1) return false;
  String rs = a.substring(0, sp1);
  String gs = a.substring(sp1 + 1, sp2);
  String bs = a.substring(sp2 + 1);
  bs.trim();
  int sp3 = bs.indexOf(' ');
  if (sp3 >= 0) bs = bs.substring(0, sp3);
  if (!isLedByteToken(rs) || !isLedByteToken(gs) || !isLedByteToken(bs)) return false;
  r = (uint8_t)rs.toInt();
  g = (uint8_t)gs.toInt();
  b = (uint8_t)bs.toInt();
  return true;
}

bool isOwner(const String& authorId) {
  return authorId == OWNER_ID_STR;
}
// ====== PUBLIC HTTP APIs (Discord commands) ======
bool getWeather(const String& zip, String& outReport) {
  WiFiClient client;
  String url = "/data/2.5/weather?zip=" + zip + ",US&units=imperial&appid=" + WEATHER_API_KEY;
  uint8_t openErr = httpGetOpen(client, "api.openweathermap.org", url, 5000);
  if (openErr) {
    setHttpOpenError(outReport, openErr, "Weather service");
    return false;
  }
  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, client);
  client.stop();
  if (err) {
    outReport = "Weather JSON parse error.";
    return false;
  }
  String city = doc["name"] | "Unknown";
  float tempF = doc["main"]["temp"] | 0.0f;
  float tempC = (tempF - 32.0f) * 5.0f / 9.0f;
  int humidity = doc["main"]["humidity"] | 0;
  String cond = doc["weather"][0]["description"] | "Unknown";
  outReport = "☁️ **Weather Report (" + city + " - " + zip + "):**\n" +
              "• **Condition:** " + cond + "\n" +
              "• **Temperature:** " + String(tempF, 1) + "°F (" + String(tempC, 1) + "°C)\n" +
              "• **Humidity:** " + String(humidity) + "%";
  return true;
}
bool skipHttpHeaders(Client& client, unsigned long timeoutMs) {
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > timeoutMs) {
      return false;
    }
  }
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) return true;
    if (millis() - timeout > timeoutMs) return false;
  }
  return true;
}

bool httpsConnect(const char* host, uint32_t timeoutMs) {
  httpsClient.stop();
  httpsClient.setInsecure();
  httpsClient.setTimeout(timeoutMs);
  return httpsClient.connect(host, 443);
}

// Returns 0=ok, 1=connect failed, 2=header timeout.
uint8_t httpsGetOpen(const char* host, const String& path, unsigned long headerTimeoutMs,
                     const char* userAgent, const char* extraHeaders) {
  if (!httpsConnect(host)) return 1;
  String req = String("GET ") + path + " HTTP/1.1\r\n"
               "Host: " + host + "\r\n"
               "User-Agent: " + userAgent + "\r\n";
  if (extraHeaders && extraHeaders[0]) req += extraHeaders;
  req += "Connection: close\r\n\r\n";
  httpsClient.print(req);
  if (!skipHttpHeaders(httpsClient, headerTimeoutMs)) {
    httpsClient.stop();
    return 2;
  }
  return 0;
}

uint8_t httpGetOpen(WiFiClient& client, const char* host, const String& path, unsigned long headerTimeoutMs) {
  if (!client.connect(host, 80)) return 1;
  client.print(String("GET ") + path + " HTTP/1.1\r\n"
               "Host: " + host + "\r\n"
               "Connection: close\r\n\r\n");
  if (!skipHttpHeaders(client, headerTimeoutMs)) {
    client.stop();
    return 2;
  }
  return 0;
}

void setHttpOpenError(String& outReport, uint8_t err, const char* label) {
  outReport = String(label);
  outReport += (err == 2) ? " timeout." : " connection failed.";
}

bool httpsAwaitHeaders(unsigned long deadlineMs, bool pump, String& outStatus,
                       bool& chunked, int& contentLength) {
  while (httpsClient.available() == 0) {
    if (millis() > deadlineMs) {
      httpsClient.stop();
      return false;
    }
    if (pump) pumpGateway();
    delay(10);
  }
  outStatus = httpsClient.readStringUntil('\n');
  outStatus.trim();
  chunked = false;
  contentLength = -1;
  while (millis() <= deadlineMs) {
    String line = httpsClient.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) break;
    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") >= 0) {
      chunked = true;
    }
    if (lower.startsWith("content-length:")) {
      contentLength = lower.substring(lower.indexOf(':') + 1).toInt();
    }
  }
  return true;
}

bool discordIdLooksValid(const String& id) {
  if (id.length() < 16) return false;
  for (unsigned int i = 0; i < id.length(); i++) {
    char c = id.charAt(i);
    if (c < '0' || c > '9') return false;
  }
  return true;
}

bool discordRestGet(const String& path, String& outBody, String& outStatus) {
  outBody = "";
  if (!httpsConnect("discord.com", 15000)) {
    outStatus = "connect failed";
    return false;
  }
  String request =
    "GET " + path + " HTTP/1.1\r\n"
    "Host: discord.com\r\n"
    "Authorization: Bot " + String(BOT_TOKEN) + "\r\n"
    "Accept: application/json\r\n"
    "Accept-Encoding: identity\r\n"
    "User-Agent: MiniMeBot/1.0\r\n"
    "Connection: close\r\n\r\n";
  httpsClient.print(request);

  unsigned long deadline = millis() + 15000UL;
  bool chunked = false;
  int contentLength = -1;
  if (!httpsAwaitHeaders(deadline, false, outStatus, chunked, contentLength)) {
    outStatus = "timeout";
    return false;
  }
  bool ok = readHttpBodyAfterHeaders(httpsClient, chunked, contentLength, outBody, deadline);
  httpsClient.stop();
  return ok;
}

String guildIdFromChannel(const String& channelId) {
  if (!discordIdLooksValid(channelId)) return "";

  String body, status;
  if (!discordRestGet("/api/v10/channels/" + channelId, body, status)) {
    return "";
  }
  int jsonStart = body.indexOf('{');
  if (jsonStart < 0) {
    return "";
  }
  if (jsonStart > 0) body = body.substring(jsonStart);

  StaticJsonDocument<64> filter;
  filter["guild_id"] = true;
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    return "";
  }
  String gid = doc["guild_id"] | "";
  return gid;
}

void rememberGuildId(const String& gid) {
  String id = gid;
  id.trim();
  if (!discordIdLooksValid(id)) return;
  for (uint8_t i = 0; i < cachedGuildCount; i++) {
    if (cachedGuildIds[i] == id) return;
  }
  if (cachedGuildCount >= 3) return;
  cachedGuildIds[cachedGuildCount++] = id;
}

bool appendMembersFromGuild(const String& guildId, uint8_t maxToAdd) {
  if (!discordIdLooksValid(guildId)) return false;

  String body, status;
  String path = "/api/v10/guilds/" + guildId + "/members?limit=200";
  if (!discordRestGet(path, body, status)) {
    return false;
  }

  int jsonStart = body.indexOf('[');
  int objStart = body.indexOf('{');
  if (jsonStart < 0 || (objStart >= 0 && objStart < jsonStart)) {
    return false;
  }
  if (jsonStart > 0) body = body.substring(jsonStart);

  StaticJsonDocument<256> filter;
  filter[0]["nick"] = true;
  filter[0]["user"]["id"] = true;
  filter[0]["user"]["username"] = true;
  filter[0]["user"]["global_name"] = true;
  filter[0]["user"]["bot"] = true;

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    return false;
  }

  JsonArray members = doc.as<JsonArray>();
  if (members.isNull()) {
    return false;
  }

  uint8_t added = 0;
  for (JsonObject member : members) {
    if (added >= maxToAdd) break;
    int slot = findFreeTrackedSlot();
    if (slot < 0) break;
    if (member["user"]["bot"] == true) continue;
    String uid = member["user"]["id"] | "";
    String name = member["nick"] | "";
    if (name.length() == 0) name = discordDisplayName(member["user"]);
    if (uid.length() == 0 || name.length() == 0) continue;
    if (findUserIndex(uid) >= 0) continue;
    fillTrackedSlot((uint8_t)slot, uid, name);
    added++;
  }

  return added > 0;
}

bool fetchGuildMembersAtStartup() {
  initTrackedUsers();
  cachedGuildCount = 0;
  rememberGuildId(String(BOT_GUILD_ID));
  rememberGuildId(guildIdFromChannel(TARGET_CHANNEL_ID));
  rememberGuildId(guildIdFromChannel(TARGET_CHANNEL_ID1));

  if (cachedGuildCount == 0) {
    return false;
  }

  bool any = false;
  uint8_t share = (cachedGuildCount > 0) ? (MAX_TRACKED_USERS / cachedGuildCount) : MAX_TRACKED_USERS;
  if (share < 1) share = 1;
  for (uint8_t g = 0; g < cachedGuildCount; g++) {
    if (appendMembersFromGuild(cachedGuildIds[g], share)) any = true;
  }
  for (uint8_t g = 0; g < cachedGuildCount; g++) {
    if (appendMembersFromGuild(cachedGuildIds[g], MAX_TRACKED_USERS)) any = true;
  }
  return any;
}

// ====== SCIENCE / AI HTTP HELPERS ======
String collapseWhitespace(String s) {
  s.replace("\n", " ");
  s.replace("\r", " ");
  s.replace("\t", " ");
  while (s.indexOf("  ") >= 0) {
    s.replace("  ", " ");
  }
  s.trim();
  return s;
}
String truncateText(const String& s, int maxLen) {
  if (s.length() <= maxLen) return s;
  return s.substring(0, maxLen - 3) + "...";
}

bool getScienceNews(String& outReport) {
  uint8_t openErr = httpsGetOpen("api.spaceflightnewsapi.net", "/v4/articles/?limit=3", 8000);
  if (openErr) {
    setHttpOpenError(outReport, openErr, "Science news");
    return false;
  }
  StaticJsonDocument<192> filter;
  filter["results"][0]["title"] = true;
  filter["results"][0]["news_site"] = true;
  filter["results"][0]["url"] = true;
  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, httpsClient, DeserializationOption::Filter(filter));
  httpsClient.stop();
  if (err) {
    outReport = "Science news JSON parse error.";
    return false;
  }
  JsonArray results = doc["results"].as<JsonArray>();
  if (results.isNull() || results.size() == 0) {
    outReport = "No science headlines right now.";
    return false;
  }
  outReport = "🛰️ **Space & high-tech headlines:**\n";
  int n = 0;
  for (JsonObject item : results) {
    if (n >= 3) break;
    String title = collapseWhitespace(item["title"] | "Untitled");
    String site = item["news_site"] | "Source";
    String url = item["url"] | "";
    outReport += String(n + 1) + ". **" + truncateText(title, 140) + "** (" + site + ")";
    if (url.length()) outReport += "\n" + url;
    outReport += "\n";
    n++;
  }
  return n > 0;
}
bool getPhysicsPapers(String& outReport) {
  const char* path =
    "/api/query?search_query=cat:physics.*&start=0&max_results=3&sortBy=submittedDate&sortOrder=descending";
  uint8_t openErr = httpsGetOpen("export.arxiv.org", path, 8000,
                                 "MiniMeBot/1.0 (ESP32 Discord bot)", "Accept-Encoding: identity\r\n");
  if (openErr) {
    setHttpOpenError(outReport, openErr, "arXiv");
    return false;
  }
  String xml;
  unsigned long start = millis();
  while (millis() - start < 8000) {
    while (httpsClient.available()) {
      xml += httpsClient.readString();
      if (xml.length() > 24000) break;
    }
    if (!httpsClient.connected() && !httpsClient.available()) break;
    delay(10);
  }
  httpsClient.stop();
  if (xml.length() < 50) {
    outReport = "arXiv response empty.";
    return false;
  }
  outReport = "⚛️ **Latest arXiv physics papers:**\n";
  int from = 0;
  int n = 0;
  while (n < 3) {
    int entry = xml.indexOf("<entry>", from);
    if (entry < 0) break;
    int entryEnd = xml.indexOf("</entry>", entry);
    if (entryEnd < 0) break;
    String block = xml.substring(entry, entryEnd);
    int t0 = block.indexOf("<title>");
    int t1 = block.indexOf("</title>");
    String title = "Untitled";
    if (t0 >= 0 && t1 > t0) {
      title = collapseWhitespace(block.substring(t0 + 7, t1));
    }
    int i0 = block.indexOf("<id>");
    int i1 = block.indexOf("</id>");
    String id = "";
    if (i0 >= 0 && i1 > i0) {
      id = collapseWhitespace(block.substring(i0 + 4, i1));
    }
    outReport += String(n + 1) + ". **" + truncateText(title, 140) + "**";
    if (id.length()) outReport += "\n" + id;
    outReport += "\n";
    n++;
    from = entryEnd + 8;
  }
  if (n == 0) {
    outReport = "No physics papers found.";
    return false;
  }
  return true;
}
bool getApod(String& outReport) {
  String path = String("/planetary/apod?api_key=") + NASA_API_KEY;
  uint8_t openErr = httpsGetOpen("api.nasa.gov", path, 8000);
  if (openErr) {
    setHttpOpenError(outReport, openErr, "NASA APOD");
    return false;
  }
  StaticJsonDocument<128> filter;
  filter["title"] = true;
  filter["explanation"] = true;
  filter["date"] = true;
  filter["url"] = true;
  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, httpsClient, DeserializationOption::Filter(filter));
  httpsClient.stop();
  if (err) {
    outReport = "NASA APOD JSON parse error.";
    return false;
  }
  String title = doc["title"] | "Astronomy Picture of the Day";
  String date = doc["date"] | "";
  String expl = collapseWhitespace(doc["explanation"] | "");
  String url = doc["url"] | "";
  outReport = "🌌 **NASA APOD";
  if (date.length()) outReport += " (" + date + ")";
  outReport += ":**\n• **" + title + "**\n" + truncateText(expl, 350);
  if (url.length()) outReport += "\n" + url;
  return true;
}
bool getIssPosition(String& outReport) {
  WiFiClient client;
  uint8_t openErr = httpGetOpen(client, "api.open-notify.org", "/iss-now.json", 5000);
  if (openErr) {
    setHttpOpenError(outReport, openErr, "ISS tracker");
    return false;
  }
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, client);
  client.stop();
  if (err) {
    outReport = "ISS tracker JSON parse error.";
    return false;
  }
  String lat = doc["iss_position"]["latitude"] | "?";
  String lon = doc["iss_position"]["longitude"] | "?";
  outReport = "🌍 **ISS now:**\n"
              "• **Latitude:** " + lat + "\n"
              "• **Longitude:** " + lon;
  return true;
}
bool askNeedPost = false;
String askPendingQuestion;
String askPendingChannelId;

bool readHttpBodyAfterHeaders(Client& client, bool chunked, int contentLength,
                              String& outBody, unsigned long deadlineMs) {
  outBody = "";
  const size_t maxBody = 48000;
  if (chunked) {
    while (millis() < deadlineMs) {
      while (!client.available() && client.connected() && millis() < deadlineMs) {
        pumpGateway();
        delay(5);
      }
      if (!client.available()) break;
      String sizeLine = client.readStringUntil('\n');
      sizeLine.trim();
      if (sizeLine.length() == 0) continue;
      int sc = sizeLine.indexOf(';');
      if (sc >= 0) sizeLine = sizeLine.substring(0, sc);
      long chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
      if (chunkSize <= 0) break;
      long got = 0;
      while (got < chunkSize && millis() < deadlineMs) {
        if (client.available()) {
          outBody += (char)client.read();
          got++;
          if (outBody.length() >= maxBody) return true;
        } else if (!client.connected()) {
          break;
        } else {
          pumpGateway();
          delay(1);
        }
      }
      client.readStringUntil('\n');
    }
    return outBody.length() > 0;
  }
  if (contentLength > 0) {
    while ((int)outBody.length() < contentLength && millis() < deadlineMs) {
      while (client.available()) {
        outBody += (char)client.read();
        if ((int)outBody.length() >= contentLength || outBody.length() >= maxBody) break;
      }
      if (!client.connected() && !client.available()) break;
      pumpGateway();
      delay(5);
    }
    return outBody.length() > 0;
  }
  while (millis() < deadlineMs) {
    while (client.available()) {
      outBody += (char)client.read();
      if (outBody.length() >= maxBody) return true;
    }
    if (!client.connected() && !client.available()) break;
    pumpGateway();
    delay(10);
  }
  return outBody.length() > 0;
}
bool askDeepSeek(const String& question, String& outReport) {
  if (DEEPSEEK_API_KEY == nullptr || strlen(DEEPSEEK_API_KEY) == 0 ||
      strcmp(DEEPSEEK_API_KEY, "DEEPSEEK_API_KEY") == 0) {
    outReport = "DeepSeek API key not set. Add DEEPSEEK_API_KEY in the sketch.";
    return false;
  }
  String q = question;
  q.trim();
  if (q.length() == 0) {
    outReport = "Usage: !ask <question>";
    return false;
  }
  if (q.length() > 500) {
    q = q.substring(0, 500);
  }
  StaticJsonDocument<1536> req;
  req["model"] = "deepseek-chat";
  req["max_tokens"] = DEEPSEEK_MAX_TOKENS;
  req["temperature"] = 0.7;
  req["stream"] = false;
  JsonArray messages = req.createNestedArray("messages");
  JsonObject sys = messages.createNestedObject();
  sys["role"] = "system";
  sys["content"] = "You are MiniMe on an ESP32 Discord bot. Answer clearly for science, tech, and physics. Keep the full answer under 2000 characters so it fits one Discord message.";
  JsonObject user = messages.createNestedObject();
  user["role"] = "user";
  user["content"] = q;
  String body;
  serializeJson(req, body);
  if (!httpsConnect("api.deepseek.com", 25000)) {
    outReport = "DeepSeek connection failed.";
    return false;
  }
  String request =
    "POST /chat/completions HTTP/1.1\r\n"
    "Host: api.deepseek.com\r\n"
    "Authorization: Bearer " + String(DEEPSEEK_API_KEY) + "\r\n"
    "Content-Type: application/json\r\n"
    "Accept: application/json\r\n"
    "Accept-Encoding: identity\r\n"
    "User-Agent: MiniMeBot/1.0\r\n"
    "Content-Length: " + String(body.length()) + "\r\n"
    "Connection: close\r\n\r\n" +
    body;
  httpsClient.print(request);
  unsigned long deadline = millis() + 45000UL;
  String statusLine;
  bool chunked = false;
  int contentLength = -1;
  if (!httpsAwaitHeaders(deadline, true, statusLine, chunked, contentLength)) {
    outReport = "DeepSeek timeout waiting for headers.";
    return false;
  }
  String respBody;
  if (!readHttpBodyAfterHeaders(httpsClient, chunked, contentLength, respBody, deadline)) {
    httpsClient.stop();
    outReport = "DeepSeek empty response. " + statusLine;
    return false;
  }
  httpsClient.stop();
  int jsonStart = respBody.indexOf('{');
  if (jsonStart < 0) {
    outReport = "DeepSeek: no JSON body. " + truncateText(statusLine, 80);
    return false;
  }
  if (jsonStart > 0) {
    respBody = respBody.substring(jsonStart);
  }
  StaticJsonDocument<128> filter;
  filter["choices"][0]["message"]["content"] = true;
  filter["error"]["message"] = true;
  StaticJsonDocument<DEEPSEEK_JSON_DOC> doc;
  DeserializationError err = deserializeJson(doc, respBody, DeserializationOption::Filter(filter));
  if (err) {
    outReport = "DeepSeek JSON parse error (" + String(err.c_str()) + ").";
    return false;
  }
  if (doc.containsKey("error")) {
    String emsg = doc["error"]["message"] | "API error";
    outReport = "DeepSeek error: " + truncateText(emsg, 200);
    return false;
  }
  String answer = collapseWhitespace(doc["choices"][0]["message"]["content"] | "");
  if (answer.length() == 0) {
    int ckey = respBody.indexOf("\"content\"");
    if (ckey >= 0) {
      int colon = respBody.indexOf(':', ckey);
      int q1 = respBody.indexOf('"', colon + 1);
      if (q1 >= 0) {
        String extracted;
        for (int i = q1 + 1; i < (int)respBody.length(); i++) {
          char c = respBody[i];
          if (c == '\\' && i + 1 < (int)respBody.length()) {
            char n = respBody[i + 1];
            if (n == 'n') { extracted += ' '; i++; continue; }
            if (n == '"' || n == '\\') { extracted += n; i++; continue; }
          }
          if (c == '"') break;
          extracted += c;
          if (extracted.length() > 1900) break;
        }
        answer = collapseWhitespace(extracted);
      }
    }
  }
  if (answer.length() == 0) {
    outReport = "DeepSeek returned an empty answer. " + truncateText(statusLine, 60);
    return false;
  }
  const char* prefix = "🧠 **DeepSeek:**\n";
  int room = DISCORD_CONTENT_MAX - (int)strlen(prefix);
  if (room < 100) room = 100;
  outReport = String(prefix) + truncateText(answer, room);
  return true;
}
void runAskFromLoop() {
  if (!askNeedPost) return;
  askNeedPost = false;
  String channelId = askPendingChannelId;
  String report;
  httpsInUse = true; // keep Gateway from starting other HTTPS during DeepSeek
  bool ok = askDeepSeek(askPendingQuestion, report);
  httpsInUse = false;
  askPendingQuestion = "";
  askPendingChannelId = "";
  if (!sendDiscordMessage(channelId, report)) {
    String fallback = ok
      ? "DeepSeek answered, but Discord rejected the post (try a shorter question)."
      : truncateText(report, DISCORD_CONTENT_MAX);
    if (!sendDiscordMessage(channelId, fallback)) {
      showTransient("DeepSeek", "Post fail");
      return;
    }
  }
  if (ok) {
    showTransient("DeepSeek", "Sent");
  } else {
    showTransient("DeepSeek", "Error");
  }
}

// ====== DISCORD COMMAND DISPATCH ======
typedef bool (*FetchReportFn)(String&);

void sendFetchResult(const String& channelId, const char* label, bool ok, const String& report,
                     const String& okLine2 = "Sent", const String& okLine3 = "") {
  sendDiscordMessage(channelId, report);
  if (ok) showTransient(label, okLine2, okLine3);
  else showTransient(label, "Error");
}

void runFetchCommand(const String& channelId, const char* label, const char* fetching,
                     FetchReportFn fetch) {
  String report;
  showTransient(label, fetching);
  sendFetchResult(channelId, label, fetch(report), report);
}

void handleCommand(const String& content, const String& authorId, const String& authorName,
                   const String& channelId, bool isDM)
{
  if (!isDM && channelId != TARGET_CHANNEL_ID && channelId != TARGET_CHANNEL_ID1) {
    return;
  }
  String raw = content;
  raw.trim();
  bool midLine = false;
  if (!raw.startsWith("!")) {
    int bang = -1;
    for (int i = 0; i < raw.length(); i++) {
      if (raw.charAt(i) != '!') continue;
      char next = (i + 1 < raw.length()) ? raw.charAt(i + 1) : 0;
      if (!((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z'))) continue;
      if (i > 0) {
        char prev = raw.charAt(i - 1);
        if (prev != ' ' && prev != '\t' && prev != '\n') continue;
      }
      bang = i;
      break;
    }
    if (bang < 0) return;
    raw = raw.substring(bang);
    midLine = true;
  }

  noteBotActivity();
  int spIdx = raw.indexOf(' ');
  String cmdWord = (spIdx > 0) ? raw.substring(0, spIdx) : raw;
  String args = (spIdx > 0) ? raw.substring(spIdx + 1) : "";
  cmdWord.toLowerCase();
  args.trim();
  // Mid-line bang: one-word args for most cmds. !ask / !display / !led keep multi-word args.
  if (midLine && args.length() > 0 &&
      cmdWord != "!ask" && cmdWord != "!display" && cmdWord != "!led") {
    int argSp = args.indexOf(' ');
    if (argSp > 0) args = args.substring(0, argSp);
    while (args.length() > 0) {
      char last = args.charAt(args.length() - 1);
      if (last == '.' || last == ',' || last == '!' || last == '?' || last == ';' || last == ':') {
        args.remove(args.length() - 1);
      } else {
        break;
      }
    }
  }

  recordUserUse(authorId, authorName);

  if (cmdWord == "!help") {
    String helpMsg =
      "🤖 **MiniMe Bot Commands**\n\n"
      "**👤 Public Commands:**\n"
      "• `!apod` — NASA Astronomy Picture of the Day.\n"
      "• `!ask <question>` — Asks DeepSeek (text AI reply in chat).\n"
      "• `!display <text>` — Writes custom text to the OLED screen.\n"
      "• `!help` — Shows this command list.\n"
      "• `!iss` — Current International Space Station position.\n"
      "• `!news` — Space and high-tech science headlines.\n"
      "• `!physics` — Latest arXiv physics papers.\n"
      "• `!sysinfo` — Displays system diagnostics (uptime, heap, RSSI, etc.).\n"
      "• `!temp` — Reads the current indoor temperature sensor.\n"
      "• `!time` — Displays the current bot time.\n"
      "• `!weather <zip>` — Fetches the weather report for a US ZIP code.\n\n"
      "**👑 Owner-Only Commands:**\n"
      "• `!led on/off` / `!led <r> <g> <b>` — RGB NeoPixel (0–255 per channel).\n"
      "• `!servo <0-90>` — Moves the servo motor to a specific angle.\n"
      "• `!set1 on` / `!set1 off` — Controls digital output pin 1.\n"
      "• `!set2 on` / `!set2 off` — Controls digital output pin 2.";
    sendDiscordMessage(channelId, helpMsg);
    showTransient("Help", "Command Sent");
    return;
  }
  if (cmdWord == "!weather") {
    if (args.length() == 0) {
      sendDiscordMessage(channelId, "Usage: !weather <zip>");
      return;
    }
    String zip = args;
    bool validZip = (zip.length() == 5);
    if (validZip) {
      for (unsigned i = 0; i < 5; i++) {
        char c = zip.charAt(i);
        if (c < '0' || c > '9') { validZip = false; break; }
      }
    }
    if (!validZip) {
      sendDiscordMessage(channelId, "Invalid ZIP code.");
      return;
    }
    String report;
    showTransient("Weather", "Fetching " + zip + "...");
    bool ok = getWeather(zip, report);
    sendFetchResult(channelId, "Weather", ok, report, zip, "Sent");
    return;
  }
  if (cmdWord == "!news") {
    runFetchCommand(channelId, "News", "Fetching...", getScienceNews);
    return;
  }
  if (cmdWord == "!physics") {
    runFetchCommand(channelId, "Physics", "Fetching arXiv...", getPhysicsPapers);
    return;
  }
  if (cmdWord == "!apod") {
    runFetchCommand(channelId, "APOD", "Fetching NASA...", getApod);
    return;
  }
  if (cmdWord == "!iss") {
    runFetchCommand(channelId, "ISS", "Fetching...", getIssPosition);
    return;
  }
  if (cmdWord == "!temp") {
    float c, f;
    if (readTemperature(c, f)) {
      dashTempC = c;
      dashTempF = f;
      String msg = "Current Temp: " + String(c, 1) + "°C / " + String(f, 1) + "°F";
      sendDiscordMessage(channelId, msg);
      showTransient("Temp", String(f, 1) + "F/" + String(c, 1) + "C");
    } else {
      sendDiscordMessage(channelId, "Temperature sensor error.");
      showTransient("Temp", "Sensor error");
    }
    return;
  }
  if (cmdWord == "!sysinfo") {
    sendDiscordMessage(channelId, getSystemInfo(), true);
    showTransient("SysInfo", "Sent");
    return;
  }
  if (cmdWord == "!time") {
    updateLocalTime();
    String currentTime = timeClient.getFormattedTime();
    String msg = "🕒 Current Bot Time: " + currentTime;
    sendDiscordMessage(channelId, msg);
    showTransient("Time", currentTime);
    return;
  }
  if (cmdWord == "!ask") {
    if (args.length() == 0) {
      sendDiscordMessage(channelId, "Usage: !ask <question>");
      return;
    }
    if (askNeedPost) {
      sendDiscordMessage(channelId, "DeepSeek is already answering. Try again in a moment.");
      return;
    }
    String question = args;
    askPendingQuestion = question;
    askPendingChannelId = channelId;
    askNeedPost = true;
    showTransient("DeepSeek", "Queued");
    return;
  }
  if (cmdWord == "!display") {
    if (args.length() == 0) {
      sendDiscordMessage(channelId, "Usage: !display <text>");
      return;
    }
    String text = args;
    if (text.length() > 50) text = text.substring(0, 50);
    String line15 = text.substring(0, text.length() > 25 ? 25 : text.length());
    String line16 = text.length() > 25 ? text.substring(25) : "";
    showTransient(line15, line16, "", 6000);
    sendDiscordMessage(channelId, "Display updated.");
    return;
  }
  int setN = 0;
  if (cmdWord.length() == 5 && cmdWord.startsWith("!set")) {
    char d = cmdWord.charAt(4);
    if (d == '1' || d == '2') setN = d - '0';
  }
  if (cmdWord == "!led" || setN != 0 || cmdWord == "!servo") {
    if (!isOwner(authorId)) {
      if (!isDM) {
        sendDiscordMessage(channelId, "You are not allowed to use this command.");
      }
      return;
    }
    if (cmdWord == "!led") {
      String a = args;
      a.trim();
      String al = a;
      al.toLowerCase();
      if (al == "on") {
        setLedRgb(255, 255, 255);
        sendDiscordMessage(channelId, "LED ON (255 255 255)");
        showTransient("LED", "ON");
      } else if (al == "off") {
        setLedRgb(0, 0, 0);
        sendDiscordMessage(channelId, "LED OFF");
        showTransient("LED", "OFF");
      } else {
        uint8_t r, g, b;
        if (!parseRgbTriplet(a, r, g, b)) {
          sendDiscordMessage(channelId, "Usage: !led on/off  or  !led <r> <g> <b> (0-255)");
          return;
        }
        setLedRgb(r, g, b);
        String msg = "LED RGB " + String(r) + " " + String(g) + " " + String(b);
        sendDiscordMessage(channelId, msg);
        showTransient("LED", String(r) + "," + String(g) + "," + String(b));
      }
      return;
    }
    if (setN != 0) {
      String a = args;
      a.toLowerCase();
      int pin = (setN == 1) ? PIN_SET1 : PIN_SET2;
      if (a != "on" && a != "off") {
        sendDiscordMessage(channelId, "Usage: !set1 on/off  or  !set2 on/off");
        return;
      }
      bool on = (a == "on");
      digitalWrite(pin, on ? HIGH : LOW);
      String label = "set" + String(setN);
      String val = on ? "ON" : "OFF";
      sendDiscordMessage(channelId, label + " " + val);
      showTransient(label, val);
      return;
    }
    if (cmdWord == "!servo") {
      if (args.length() == 0) {
        sendDiscordMessage(channelId, "Usage: !servo <0-90>");
        return;
      }
      int angle = args.toInt();
      if (angle < 0 || angle > 90) {
        sendDiscordMessage(channelId, "Angle out of range. Allowed: 0-90 degrees.");
        return;
      }
      setServoAngle(angle);
      String msg = "Servo set to " + String(angle) + " degrees.";
      sendDiscordMessage(channelId, msg);
      showTransient("Servo", String(angle) + " deg");
      return;
    }
  }

  sendDiscordMessage(channelId, "That is not a command.");
  showTransient("Unknown", cmdWord);
}
// ====== DISCORD GATEWAY WEBSOCKET ======
void gatewayEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      gatewayConnected = false;
      identified       = false;
      botDiscordStatus = 0;
      showTransient("Gateway", "Disconnected");
      break;
    case WStype_CONNECTED:
      gatewayConnected = true;
      showTransient("Gateway", "Connected");
      break;
    case WStype_TEXT: {
      static StaticJsonDocument<384> gwFilter;
      static bool gwFilterInit = false;
      if (!gwFilterInit) {
        gwFilter["op"] = true;
        gwFilter["s"] = true;
        gwFilter["t"] = true;
        gwFilter["d"]["heartbeat_interval"] = true;
        gwFilter["d"]["status"] = true;
        gwFilter["d"]["user"]["id"] = true;
        gwFilter["d"]["user"]["username"] = true;
        gwFilter["d"]["user"]["global_name"] = true;
        gwFilter["d"]["content"] = true;
        gwFilter["d"]["channel_id"] = true;
        gwFilter["d"]["guild_id"] = true;
        gwFilter["d"]["author"]["id"] = true;
        gwFilter["d"]["author"]["username"] = true;
        gwFilter["d"]["author"]["global_name"] = true;
        gwFilter["d"]["author"]["bot"] = true;
        gwFilter["d"]["presences"][0]["user"]["id"] = true;
        gwFilter["d"]["presences"][0]["status"] = true;
        gwFilter["d"]["guilds"][0]["presences"][0]["user"]["id"] = true;
        gwFilter["d"]["guilds"][0]["presences"][0]["status"] = true;
        gwFilterInit = true;
      }

      if (!gwDoc) return;
      gwDoc->clear();
      DeserializationError err = deserializeJson(*gwDoc, payload, length, DeserializationOption::Filter(gwFilter));
      if (err) return;
      int op = (*gwDoc)["op"] | -1;
      if (gwDoc->containsKey("s") && !(*gwDoc)["s"].isNull()) {
        lastSeq = (*gwDoc)["s"].as<int>();
      }
      if (op == 10) {
        heartbeatIntervalMs = (*gwDoc)["d"]["heartbeat_interval"] | 0;
        lastHeartbeatMillis = millis();
        sendIdentify();
        identified = true;
        return;
      }
      if (op == 11) return;
      if (op == 0) {
        const char* t = (*gwDoc)["t"];
        if (!t) return;
        if (strcmp(t, "READY") == 0) {
          JsonArray guilds = (*gwDoc)["d"]["guilds"].as<JsonArray>();
          if (!guilds.isNull()) {
            for (JsonObject g : guilds) {
              applyPresencesArray(g["presences"].as<JsonArray>());
            }
          }
          requestTrackedUserPresences();
          return;
        }
        if (strcmp(t, "GUILD_CREATE") == 0) {
          applyPresencesArray((*gwDoc)["d"]["presences"].as<JsonArray>());
          requestTrackedUserPresences();
          return;
        }
        if (strcmp(t, "GUILD_MEMBERS_CHUNK") == 0) {
          applyPresencesArray((*gwDoc)["d"]["presences"].as<JsonArray>());
          return;
        }
        if (strcmp(t, "PRESENCE_UPDATE") == 0) {
          handlePresenceUpdate((*gwDoc)["d"]);
          return;
        }
        if (strcmp(t, "MESSAGE_CREATE") == 0) {
          if (httpsInUse) return; // DeepSeek holds HTTPS; defer commands until free
          JsonObject d = (*gwDoc)["d"];
          if (d["author"]["bot"] == true) return;
          String content   = d["content"].as<String>();
          String channelId = d["channel_id"].as<String>();
          String authorId  = d["author"]["id"].as<String>();
          String authorName = discordDisplayName(d["author"]);
          bool isDM = d["guild_id"].isNull();
          handleCommand(content, authorId, authorName, channelId, isDM);
        }
      }
      break;
    }
    default:
      break;
  }
}
// ====== SCHEDULED POSTS (loop) ======
void backgroundTasks() {
  runAskFromLoop();
  unsigned long now = millis();
  // Wait for Gateway so boot sysinfo is not "Disconnected"
  if (gatewayConnected && identified &&
      (lastSysInfoMillis == 0 || now - lastSysInfoMillis >= SYSINFO_INTERVAL_MS)) {
    lastSysInfoMillis = now;
    noteBotActivity();
    sendDiscordMessage(TARGET_CHANNEL_ID, getSystemInfo(), true);
  }
  updateLocalTime();
  int currentHour = timeClient.getHours();
  int currentMinute = timeClient.getMinutes();
  if ((currentHour == 6 || currentHour == 12 || currentHour == 18) && currentMinute == 0) {
    if (lastSentHour != currentHour) {
      lastSentHour = currentHour;
      float c, f;
      noteBotActivity();
      if (readTemperature(c, f)) {
        String report = "⏰ **Scheduled Summary (" + String(currentHour) + ":00):**\n" +
                        "• **Indoor Temp:** " + String(c, 1) + "°C / " + String(f, 1) + "°F";
        sendDiscordMessage(TARGET_CHANNEL_ID, report);
        showTransient("Scheduled", "Report Sent");
      } else {
        sendDiscordMessage(TARGET_CHANNEL_ID, "⏰ **Scheduled Summary:** Temperature sensor error.");
      }
    }
  } else {
    if (lastSentHour != -1 && currentHour != 6 && currentHour != 12 && currentHour != 18) {
      lastSentHour = -1;
    }
  }
}
// ====== SETUP / LOOP ======
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  showTransient("WiFi", "Connecting...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  showTransient("WiFi", "Connected");
}

void connectGateway() {
  gatewayWS.beginSSL("gateway.discord.gg", 443, "/?v=10&encoding=json");
  gatewayWS.onEvent(gatewayEvent);
  gatewayWS.setReconnectInterval(5000);
}

void setup() {
  delay(500);
  gwDoc = new DynamicJsonDocument(GW_DOC_PSRAM);
  initTrackedUsers();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  u8g2.begin();
  u8g2.setContrast(DISPLAY_CONTRAST_FULL);
  lastDisplayActivityMillis = millis();
  showTransient("Booting...", "ESP32-S3 Discord bot");
  setupPins();
  sensors.begin();
  connectWiFi();
  timeClient.begin();
  showTransient("Discord", "Loading users...");
  if (fetchGuildMembersAtStartup()) {
    String n0 = trackedUsers[0].userName.length() ? trackedUsers[0].userName : "ok";
    showTransient("Users loaded", n0);
  } else {
    showTransient("Users", "Fetch failed");
  }
  delay(1200);
  connectGateway();
  setServoAngle(45);
  lastDashMillis = 0;
  setupTouch(); // after WiFi/I2C
  showTransient("Ready", "Idle presence");
  drawDashboard();
}

void loop() {
  pumpGateway();
  backgroundTasks();
  pollTouchWake();
  updateBotPresenceIdle();
  updateDisplay();
  applyCpuForIdleState();
  delay(5);
}
