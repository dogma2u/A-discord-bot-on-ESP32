/*
 MiniMe Bot Commands
 Public Commands:
• !weather <zip> — Fetches the weather report for a US ZIP code.
• !temp — Reads the current indoor temperature sensor.
• !sysinfo — Displays system diagnostics (uptime, heap, RSSI, etc.).
• !time — Displays the current bot time.
• !news — Space and high-tech science headlines.
• !physics — Latest arXiv physics papers.
• !apod — NASA Astronomy Picture of the Day.
• !iss — Current International Space Station position.
• !ask <question> — Asks DeepSeek (text AI reply in chat).
• !help — Shows this command list.
 Owner-Only Commands:
• !led on / !led off — Controls the RGB NeoPixel LED.
• !set1 on / !set1 off — Controls digital output pin 1.
• !set2 on / !set2 off — Controls digital output pin 2.
• !servo <0-90> — Moves the servo motor to a specific angle.
• !display <text> — Writes custom text to the OLED screen.
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
#include <esp_heap_caps.h>
// ====== USER CONFIG ======
const char* WIFI_SSID     = "ssid";
const char* WIFI_PASSWORD = "password";
const char* BOT_TOKEN     = "bot token";
const char* WEATHER_API_KEY = "WEATHER_API_KEY";
const char* NASA_API_KEY    = "NASA_API_KEY";
const char* DEEPSEEK_API_KEY = "DEEPSEEK_API_KEY";
// Guild to fetch members from at startup
#define BOT_GUILD_ID "GUILD_ID"
// ====== OWNER AND CHANNEL IDS ======
const String OWNER_ID_STR        = "OWNER_ID_STR";
const char*  TEMP_CHANNEL_ID_STR = "TEMP_CHANNEL_ID_STR";  // main channel
const String TARGET_CHANNEL_ID  = "TARGET_CHANNEL_ID";
const String TARGET_CHANNEL_ID1 = "TARGET_CHANNEL_ID1";
// ====== GPIO CONFIG ======
const int RGB_LED_PIN = 48;
const int PIN_SERVO   = 47;
const int PIN_SET1    = 6;
const int PIN_SET2    = 7;
const int PIN_DS18B20 = 10;
// I2C SSD1327
const int PIN_I2C_SDA = 8;
const int PIN_I2C_SCL = 9;
// ====== TIME CONFIG (NTP) — US Pacific, with daylight saving ======
WiFiUDP ntpUDP;
const long PST_OFFSET_SEC = -28800; // UTC-8 standard
const long PDT_OFFSET_SEC = -25200; // UTC-7 daylight
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
const unsigned long SYSINFO_INTERVAL_MS = 14400000UL; // 4 Hours
unsigned long lastSysInfoMillis = 0;
int lastSentHour = -1;
// ====== DISCORD GATEWAY ======
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
// ====== DISPLAY STATE ======
U8G2_SSD1327_EA_W128128_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
void noteDisplayActivity();

// Dashboard live data
float dashTempC = -999.0f;
float dashTempF = -999.0f;
String dashLastCmd   = "none";
String dashLastEvent = "Starting...";
unsigned long dashLastCmdMillis = 0;

// ---- User usage + presence tracking (for dashboard) ----
// Requirement you gave: each user gets their own line, show their name,
// whether they are on/idle/off, and how many times they used the bot
// in the last 24 hours (reset every 24 hours).
struct TrackedUser {
  String userId;
  String userName;
  uint8_t status;   // 0=Off, 1=Idle, 2=On
  uint32_t useCount24h;
  bool active;
};

static const uint8_t MAX_TRACKED_USERS = 5;
TrackedUser trackedUsers[MAX_TRACKED_USERS];
String cachedGuildId = "";

unsigned long usesWindowStartMillis = 0;
const unsigned long USES_WINDOW_MS = 86400UL * 1000UL; // 24 hours

uint8_t statusFromDiscord(const char* status) {
  if (!status) return 0;
  if (strcmp(status, "online") == 0) return 2;
  if (strcmp(status, "idle") == 0) return 1;
  if (strcmp(status, "dnd") == 0) return 3;
  if (strcmp(status, "offline") == 0) return 0;
  return 0;
}

const char* statusToWord(uint8_t status) {
  if (status == 2) return "On";
  if (status == 1) return "Idle";
  if (status == 3) return "DND";
  return "Off";
}

void initTrackedUsers() {
  for (uint8_t i = 0; i < MAX_TRACKED_USERS; i++) {
    trackedUsers[i].active = false;
    trackedUsers[i].userId = "";
    trackedUsers[i].userName = "";
    trackedUsers[i].status = 0;
    trackedUsers[i].useCount24h = 0;
  }
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

int addOrPickUserSlot(const String& userId, const String& userName) {
  // Return existing slot if present
  int idx = findUserIndex(userId);
  if (idx >= 0) {
    if (userName.length()) trackedUsers[idx].userName = userName;
    return idx;
  }

  // Find inactive slot
  for (uint8_t i = 0; i < MAX_TRACKED_USERS; i++) {
    if (!trackedUsers[i].active) {
      trackedUsers[i].active = true;
      trackedUsers[i].userId = userId;
      trackedUsers[i].userName = userName;
      trackedUsers[i].status = 0;
      trackedUsers[i].useCount24h = 0;
      return (int)i;
    }
  }

  // No free slot: overwrite lowest-use slot.
  uint8_t worst = 0;
  for (uint8_t i = 1; i < MAX_TRACKED_USERS; i++) {
    if (trackedUsers[i].useCount24h < trackedUsers[worst].useCount24h) worst = i;
  }
  trackedUsers[worst].active = true;
  trackedUsers[worst].userId = userId;
  trackedUsers[worst].userName = userName;
  trackedUsers[worst].status = 0;
  trackedUsers[worst].useCount24h = 0;
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
  // Command implies they're at least "On" from a user-experience perspective.
  trackedUsers[idx].status = 2;
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
    uint8_t newSt = statusFromDiscord(st);
    if (trackedUsers[idx].status != newSt) {
      trackedUsers[idx].status = newSt;
      noteDisplayActivity();
    }
    Serial.print("[PRESENCE] ");
    Serial.print(trackedUsers[idx].userName);
    Serial.print(" -> ");
    Serial.println(statusToWord(trackedUsers[idx].status));
  }
}

void requestTrackedUserPresences() {
  if (cachedGuildId.length() < 16) return;
  bool any = false;
  for (uint8_t i = 0; i < MAX_TRACKED_USERS; i++) {
    if (trackedUsers[i].active && trackedUsers[i].userId.length()) {
      any = true;
      break;
    }
  }
  if (!any) return;

  StaticJsonDocument<768> doc;
  doc["op"] = 8;
  JsonObject d = doc.createNestedObject("d");
  d["guild_id"] = cachedGuildId;
  d["limit"] = 0;
  d["presences"] = true;
  JsonArray ids = d.createNestedArray("user_ids");
  for (uint8_t i = 0; i < MAX_TRACKED_USERS; i++) {
    if (trackedUsers[i].active && trackedUsers[i].userId.length()) {
      ids.add(trackedUsers[i].userId);
    }
  }
  String payload;
  serializeJson(doc, payload);
  gatewayWS.sendTXT(payload);
  Serial.println("[GW] Request Guild Members (presences) sent");
}

void handlePresenceUpdate(JsonObject d) {
  const char* uid = d["user"]["id"];
  const char* st  = d["status"] | "offline";
  if (!uid) return;

  int idx = findUserIndex(String(uid));
  if (idx < 0) {
    String name = d["user"]["global_name"] | "";
    if (name.length() == 0) {
      name = d["user"]["username"] | "";
    }
    idx = addOrPickUserSlot(String(uid), name);
    if (idx < 0) return;
  }
  String pName = d["user"]["global_name"] | "";
  if (pName.length() == 0) {
    pName = d["user"]["username"] | "";
  }
  if (pName.length()) trackedUsers[idx].userName = pName;
  uint8_t newSt = statusFromDiscord(st);
  if (trackedUsers[idx].status != newSt) {
    trackedUsers[idx].status = newSt;
    noteDisplayActivity();
  }
}

// Transient overlay: when non-empty, show this instead of dashboard for a few seconds
String transientLine1 = "";
String transientLine2 = "";
String transientLine3 = "";
unsigned long transientUntilMs = 0;

// Dashboard refresh throttle
unsigned long lastDashMillis = 0;
const unsigned long DASH_REFRESH_MS = 2000;
unsigned long lastDisplayActivityMillis = 0;
bool displayAsleep = false;
const unsigned long DISPLAY_IDLE_MS = 600000UL; // 10 minutes
const uint8_t DISPLAY_CONTRAST_FULL = 255;

// ====== TEMP SENSOR ======
OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);
// ====== NEOPIXEL ======
Adafruit_NeoPixel pixels(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);

// ====== FORWARD DECLARATIONS ======
bool readTemperature(float &tempC, float &tempF);
bool readHttpBodyAfterHeaders(Client& client, bool chunked, int contentLength,
                              String& outBody, unsigned long deadlineMs);
bool discordIdLooksValid(const String& id);

// ====== DISPLAY UTILS ======

// (old rolling-window "last user" logic removed; replaced with multi-user + 24h reset)

// Show a brief transient message (replaces dashboard for durationMs ms)
void noteDisplayActivity() {
  lastDisplayActivityMillis = millis();
  if (displayAsleep) {
    displayAsleep = false;
    u8g2.setPowerSave(0);
    u8g2.setContrast(DISPLAY_CONTRAST_FULL);
    lastDashMillis = 0;
  }
}

void updateDisplaySleep() {
  if (displayAsleep) return;
  if (lastDisplayActivityMillis == 0) {
    lastDisplayActivityMillis = millis();
    return;
  }
  if (millis() - lastDisplayActivityMillis >= DISPLAY_IDLE_MS) {
    displayAsleep = true;
    u8g2.setPowerSave(1); // panel off until an event
  }
}

void showTransient(const String& line1, const String& line2 = "", const String& line3 = "", unsigned long durationMs = 3000) {
  noteDisplayActivity();
  transientLine1 = line1;
  transientLine2 = line2;
  transientLine3 = line3;
  transientUntilMs = millis() + durationMs;
}

// Draw the persistent dashboard (128x128 SSD1327)
// Font: u8g2_font_5x7_tf = 5px wide + 1px gap = 6px/char, 7px tall + 1px gap = 8px/row
// Grid: 16 rows x 18 chars on 128x128
// Row baselines (y = row*8, glyph baseline at y+7):
//   Row 0  y=7   Header: MiniMe + GW:good/bad + right-justified time
//   Row 1  y=8   divider line
//   Row 2  y=23  Sig bar
//   Row 3  y=31  Heap bar
//   Row 4  y=39  Temp: xx.xF / xx.xC
//   Row 5  y=47  Up Time: XdXhXm
//   Row 6  y=55  User1: Name <On|Idle|DND|Off> Count
//   Row 7  y=63  User2: Name <On|Idle|DND|Off> Count
//   Row 8  y=71  User3: Name <On|Idle|DND|Off> Count
//   Row 9  y=79  User4: Name <On|Idle|DND|Off> Count
//   Row 10 y=87  User5: Name <On|Idle|DND|Off> Count
void drawDashboard() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);

  // ---- Row 0: Header (MiniMe + GW + right-justified time) ----
  updateLocalTime();
  String t = timeClient.getFormattedTime(); // HH:MM:SS
  String gwHeader = gatewayConnected ? "GW:good" : "GW:bad";
  u8g2.drawStr(0, 7, "MiniMe");
  u8g2.drawStr(42, 7, gwHeader.c_str());
  // True right-justify using actual rendered width.
  int timeX = 128 - u8g2.getStrWidth(t.c_str());
  if (timeX < 0) timeX = 0;
  u8g2.drawStr(timeX, 7, t.c_str());

  // divider
  u8g2.drawHLine(0, 9, 128);

  // ---- Rows 2-3: Signal + Heap bars ----
  long rssi = WiFi.RSSI();
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t totalHeap = ESP.getHeapSize();

  // Signal bar (row 2)
  int sigBarW = 0;
  if (rssi >= -40) sigBarW = 79;
  else if (rssi <= -100) sigBarW = 0;
  else sigBarW = (int)((rssi + 100) * 79 / 60);
  u8g2.drawStr(0, 23, "Sig:");
  u8g2.drawFrame(25, 16, 103, 8);
  if (sigBarW > 0) u8g2.drawBox(26, 17, sigBarW, 6);

  // Heap bar (row 3) — internal SRAM + this board's 8MB PSRAM, original 8px height
  uint32_t psramSize = ESP.getPsramSize();
  uint32_t psramFree = ESP.getFreePsram();
  if (psramSize < BOARD_PSRAM_BYTES) psramSize = BOARD_PSRAM_BYTES;
  if (psramFree < 1) psramFree = BOARD_PSRAM_BYTES;
  uint32_t memTotal = totalHeap + psramSize;
  uint32_t memFree  = freeHeap + psramFree;
  int heapBarW = 0;
  if (memTotal > 0) {
    heapBarW = (int)((memFree * 79UL) / memTotal);
    if (heapBarW < 0) heapBarW = 0;
    if (heapBarW > 79) heapBarW = 79;
  }
  u8g2.drawStr(0, 31, "Heap:");
  u8g2.drawFrame(25, 24, 103, 8);
  if (heapBarW > 0) u8g2.drawBox(26, 25, heapBarW, 6);

  // ---- Row 4: Temp ----
  u8g2.drawStr(0, 39, "Temp:");
  if (dashTempC > -998.0f) {
    char buf[28];
    snprintf(buf, sizeof(buf), "%.1fF / %.1fC", dashTempF, dashTempC);
    u8g2.drawStr(31, 39, buf);
  } else {
    u8g2.drawStr(31, 39, "-- sensor --");
  }

  // ---- Row 5: Up Time ----
  unsigned long uptimeSec = millis() / 1000;
  unsigned long d = uptimeSec / 86400;
  unsigned long h = (uptimeSec % 86400) / 3600;
  unsigned long m = (uptimeSec % 3600) / 60;
  char upBuf[24];
  snprintf(upBuf, sizeof(upBuf), "Up Time:%lud%luh%lum", d, h, m);
  u8g2.drawStr(0, 47, upBuf);

  // ---- Rows 6-10: Users ----
  // 4x6 font so names keep more of the 128px width.
  // Name left, status aligned after the longest name, Bot:N right-justified.
  u8g2.setFont(u8g2_font_4x6_tf);
  const int gapPx = 4;
  const int statusW = u8g2.getStrWidth("Idle");
  const int botReserveW = u8g2.getStrWidth("Bot:999");
  int nameMaxPx = 128 - gapPx - statusW - gapPx - botReserveW;
  if (nameMaxPx < 16) nameMaxPx = 16;

  String names[MAX_TRACKED_USERS];
  int longestNamePx = 0;
  for (uint8_t i = 0; i < MAX_TRACKED_USERS; i++) {
    if (!trackedUsers[i].active) {
      names[i] = "---";
    } else if (trackedUsers[i].userName.length()) {
      names[i] = trackedUsers[i].userName;
    } else {
      names[i] = trackedUsers[i].userId;
    }
    int w = u8g2.getStrWidth(names[i].c_str());
    if (w > longestNamePx) longestNamePx = w;
  }
  if (longestNamePx > nameMaxPx) longestNamePx = nameMaxPx;
  int statusX = longestNamePx + gapPx;

  for (uint8_t row = 0; row < 5; row++) {
    uint8_t y = 55 + (row * 8);
    String name = names[row];
    while (name.length() > 1 && u8g2.getStrWidth(name.c_str()) > longestNamePx) {
      name.remove(name.length() - 1);
    }
    u8g2.drawStr(0, y, name.c_str());

    String statusWord = String(statusToWord(trackedUsers[row].active ? trackedUsers[row].status : 0));
    u8g2.drawStr(statusX, y, statusWord.c_str());

    uint32_t uses = trackedUsers[row].active ? trackedUsers[row].useCount24h : 0;
    String botStr = "Bot:" + String(uses);
    int botX = 128 - u8g2.getStrWidth(botStr.c_str());
    if (botX < statusX + statusW + 2) botX = statusX + statusW + 2;
    u8g2.drawStr(botX, y, botStr.c_str());
  }

  u8g2.sendBuffer();
}

// Draw transient overlay (3-line status message)
void drawTransient() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 16, transientLine1.c_str());
  if (transientLine2.length()) u8g2.drawStr(0, 32, transientLine2.c_str());
  if (transientLine3.length()) u8g2.drawStr(0, 48, transientLine3.c_str());
  u8g2.sendBuffer();
}

// Called from loop — manages transient vs dashboard
void updateDisplay() {
  updateDisplaySleep();
  if (displayAsleep) return;
  unsigned long now = millis();
  if (now < transientUntilMs) {
    drawTransient();
  } else if (now - lastDashMillis >= DASH_REFRESH_MS) {
    lastDashMillis = now;
    // Refresh temp reading for dashboard
    float c, f;
    if (readTemperature(c, f)) {
      dashTempC = c;
      dashTempF = f;
    }
    drawDashboard();
  }
}

// Legacy helper kept for boot/connect messages that need an immediate paint
void drawStatus(const String& line1, const String& line2 = "", const String& line3 = "") {
  showTransient(line1, line2, line3, 3000);
  drawTransient();
}

// ====== SERVO ======
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
  setupServo();
  setServoAngle(0);
}
bool readTemperature(float &tempC, float &tempF) {
  sensors.requestTemperatures();
  float c = sensors.getTempCByIndex(0);
  if (c == DEVICE_DISCONNECTED_C) {
    return false;
  }
  tempC = c;
  tempF = c * 9.0f / 5.0f + 32.0f;
  return true;
}
String getSystemInfo() {
  long rssi = WiFi.RSSI();
  uint32_t psramSize = ESP.getPsramSize();
  if (psramSize < BOARD_PSRAM_BYTES) psramSize = BOARD_PSRAM_BYTES;
  uint32_t psramFree = ESP.getFreePsram();
  if (ESP.getPsramSize() == 0) psramFree = BOARD_PSRAM_BYTES;
  uint32_t freeHeap = ESP.getFreeHeap() + psramFree;
  uint32_t totalHeap = ESP.getHeapSize() + psramSize;
  unsigned long uptimeSec = millis() / 1000;
  unsigned long days = uptimeSec / 86400;
  unsigned long hours = (uptimeSec % 86400) / 3600;
  unsigned long minutes = (uptimeSec % 3600) / 60;
  String uptimeStr = String(days) + "d " + String(hours) + "h " + String(minutes) + "m";
  return "📊 **System Diagnostics:**\n"
         "• **Uptime:** " + uptimeStr + "\n"
         "• **Free Heap:** " + String((unsigned long)freeHeap) + " / " +
         String((unsigned long)totalHeap) + " bytes\n"
         "• **WiFi RSSI:** " + String(rssi) + " dBm\n"
         "• **Gateway Status:** " + String(gatewayConnected ? "Connected" : "Disconnected");
}
bool sendDiscordMessage(const String& channelId, const String& content) {
  httpsClient.stop();
  httpsClient.setInsecure();
  if (!httpsClient.connect("discord.com", 443)) {
    Serial.println("[REST] HTTPS connect failed");
    return false;
  }
  String url = "/api/v10/channels/" + channelId + "/messages";
  StaticJsonDocument<2048> doc;
  doc["content"] = content;
  doc["tts"] = false;
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
  unsigned long start = millis();
  while (httpsClient.connected() && millis() - start < 5000) {
    if (httpsClient.available()) {
      String line = httpsClient.readStringUntil('\n');
      if (line == "\r") break;
    }
  }
  httpsClient.stop();
  return true;
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
  // Include PRESENCE_UPDATE so we can show "online" status on the display.
  d["intents"] = 37635; // GUILDS + GUILD_MEMBERS + GUILD_PRESENCES + GUILD_MESSAGES + DIRECT_MESSAGES + MESSAGE_CONTENT
  String payload;
  serializeJson(doc, payload);
  gatewayWS.sendTXT(payload);
  Serial.println("[GW] IDENTIFY sent");
}
void sendHeartbeat() {
  StaticJsonDocument<256> doc;
  doc["op"] = 1;
  if (lastSeq == 0) {
    doc["d"] = nullptr;
  } else {
    doc["d"] = lastSeq;
  }
  String payload;
  serializeJson(doc, payload);
  gatewayWS.sendTXT(payload);
  Serial.println("[GW] HEARTBEAT sent");
}
bool isOwner(const String& authorId) {
  return authorId == OWNER_ID_STR;
}
bool getWeather(const String& zip, String& outReport) {
  WiFiClient client;
  if (!client.connect("api.openweathermap.org", 80)) {
    outReport = "Weather service connection failed.";
    return false;
  }
  String url = "/data/2.5/weather?zip=" + zip + ",US&units=imperial&appid=" + WEATHER_API_KEY;
  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: api.openweathermap.org\r\n" +
               "Connection: close\r\n\r\n");
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 5000) {
      outReport = "Weather service timeout.";
      client.stop();
      return false;
    }
  }
  while (client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
  }
  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, client);
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
  httpsClient.stop();
  httpsClient.setInsecure();
  httpsClient.setTimeout(15000);
  if (!httpsClient.connect("discord.com", 443)) {
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
  while (httpsClient.available() == 0) {
    if (millis() > deadline) {
      httpsClient.stop();
      outStatus = "timeout";
      return false;
    }
    delay(10);
  }
  outStatus = httpsClient.readStringUntil('\n');
  outStatus.trim();
  bool chunked = false;
  int contentLength = -1;
  while (millis() <= deadline) {
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
  bool ok = readHttpBodyAfterHeaders(httpsClient, chunked, contentLength, outBody, deadline);
  httpsClient.stop();
  return ok;
}

String resolveGuildIdAtStartup() {
  String guildId = String(BOT_GUILD_ID);
  guildId.trim();
  if (discordIdLooksValid(guildId)) return guildId;

  if (!discordIdLooksValid(TARGET_CHANNEL_ID)) {
    Serial.println("[MEMBERS] no valid BOT_GUILD_ID or TARGET_CHANNEL_ID");
    return "";
  }

  String body, status;
  if (!discordRestGet("/api/v10/channels/" + TARGET_CHANNEL_ID, body, status)) {
    Serial.print("[MEMBERS] channel lookup failed: ");
    Serial.println(status);
    return "";
  }
  int jsonStart = body.indexOf('{');
  if (jsonStart < 0) {
    Serial.print("[MEMBERS] channel lookup: no JSON. ");
    Serial.println(status);
    return "";
  }
  if (jsonStart > 0) body = body.substring(jsonStart);

  StaticJsonDocument<64> filter;
  filter["guild_id"] = true;
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    Serial.print("[MEMBERS] channel JSON error: ");
    Serial.println(err.c_str());
    return "";
  }
  String gid = doc["guild_id"] | "";
  Serial.print("[MEMBERS] guild from channel: ");
  Serial.println(gid);
  return gid;
}

bool fetchGuildMembersAtStartup() {
  String guildId = resolveGuildIdAtStartup();
  if (!discordIdLooksValid(guildId)) {
    Serial.println("[MEMBERS] guild id missing/invalid");
    return false;
  }
  cachedGuildId = guildId;

  String body, status;
  String path = "/api/v10/guilds/" + guildId + "/members?limit=200";
  if (!discordRestGet(path, body, status)) {
    Serial.print("[MEMBERS] fetch failed: ");
    Serial.println(status);
    return false;
  }
  Serial.print("[MEMBERS] HTTP ");
  Serial.println(status);

  int jsonStart = body.indexOf('[');
  int objStart = body.indexOf('{');
  if (jsonStart < 0 || (objStart >= 0 && objStart < jsonStart)) {
    Serial.print("[MEMBERS] not a member array: ");
    Serial.println(body.substring(0, 180));
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
    Serial.print("[MEMBERS] JSON parse error: ");
    Serial.println(err.c_str());
    return false;
  }

  JsonArray members = doc.as<JsonArray>();
  if (members.isNull()) {
    Serial.println("[MEMBERS] no member array");
    return false;
  }

  initTrackedUsers();
  uint8_t slot = 0;
  for (JsonObject member : members) {
    if (slot >= MAX_TRACKED_USERS) break;
    if (member["user"]["bot"] == true) continue;
    String uid = member["user"]["id"] | "";
    String name = member["nick"] | "";
    if (name.length() == 0) name = member["user"]["global_name"] | "";
    if (name.length() == 0) name = member["user"]["username"] | "";
    if (uid.length() == 0 || name.length() == 0) continue;
    trackedUsers[slot].active = true;
    trackedUsers[slot].userId = uid;
    trackedUsers[slot].userName = name;
    trackedUsers[slot].status = 0;
    trackedUsers[slot].useCount24h = 0;
    Serial.print("[MEMBERS] ");
    Serial.print(slot);
    Serial.print(": ");
    Serial.println(name);
    slot++;
  }

  Serial.print("[MEMBERS] loaded users: ");
  Serial.println(slot);
  return slot > 0;
}

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
  httpsClient.stop();
  httpsClient.setInsecure();
  if (!httpsClient.connect("api.spaceflightnewsapi.net", 443)) {
    outReport = "Science news connection failed.";
    return false;
  }
  httpsClient.print(
    "GET /v4/articles/?limit=3 HTTP/1.1\r\n"
    "Host: api.spaceflightnewsapi.net\r\n"
    "User-Agent: MiniMeBot/1.0\r\n"
    "Connection: close\r\n\r\n");
  if (!skipHttpHeaders(httpsClient, 8000)) {
    outReport = "Science news timeout.";
    httpsClient.stop();
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
  httpsClient.stop();
  httpsClient.setInsecure();
  if (!httpsClient.connect("export.arxiv.org", 443)) {
    outReport = "arXiv connection failed.";
    return false;
  }
  httpsClient.print(
    "GET /api/query?search_query=cat:physics.*&start=0&max_results=3&sortBy=submittedDate&sortOrder=descending HTTP/1.1\r\n"
    "Host: export.arxiv.org\r\n"
    "User-Agent: MiniMeBot/1.0 (ESP32 Discord bot)\r\n"
    "Accept-Encoding: identity\r\n"
    "Connection: close\r\n\r\n");
  if (!skipHttpHeaders(httpsClient, 8000)) {
    outReport = "arXiv timeout.";
    httpsClient.stop();
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
  httpsClient.stop();
  httpsClient.setInsecure();
  if (!httpsClient.connect("api.nasa.gov", 443)) {
    outReport = "NASA APOD connection failed.";
    return false;
  }
  String path = String("/planetary/apod?api_key=") + NASA_API_KEY;
  httpsClient.print(String("GET ") + path + " HTTP/1.1\r\n" +
                    "Host: api.nasa.gov\r\n" +
                    "User-Agent: MiniMeBot/1.0\r\n" +
                    "Connection: close\r\n\r\n");
  if (!skipHttpHeaders(httpsClient, 8000)) {
    outReport = "NASA APOD timeout.";
    httpsClient.stop();
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
  if (!client.connect("api.open-notify.org", 80)) {
    outReport = "ISS tracker connection failed.";
    return false;
  }
  client.print(
    "GET /iss-now.json HTTP/1.1\r\n"
    "Host: api.open-notify.org\r\n"
    "Connection: close\r\n\r\n");
  if (!skipHttpHeaders(client, 5000)) {
    outReport = "ISS tracker timeout.";
    client.stop();
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
bool readHttpBodyAfterHeaders(Client& client, bool chunked, int contentLength,
                              String& outBody, unsigned long deadlineMs) {
  outBody = "";
  const size_t maxBody = 48000;
  if (chunked) {
    while (millis() < deadlineMs) {
      while (!client.available() && client.connected() && millis() < deadlineMs) {
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
    delay(10);
  }
  return outBody.length() > 0;
}
bool askDeepSeek(const String& question, String& outReport) {
  if (DEEPSEEK_API_KEY == nullptr || strlen(DEEPSEEK_API_KEY) == 0) {
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
  req["max_tokens"] = 220;
  req["temperature"] = 0.7;
  req["stream"] = false;
  JsonArray messages = req.createNestedArray("messages");
  JsonObject sys = messages.createNestedObject();
  sys["role"] = "system";
  sys["content"] = "You are MiniMe on an ESP32 Discord bot. Answer clearly and briefly for science, tech, and physics. Keep replies under 200 words.";
  JsonObject user = messages.createNestedObject();
  user["role"] = "user";
  user["content"] = q;
  String body;
  serializeJson(req, body);
  httpsClient.stop();
  httpsClient.setInsecure();
  httpsClient.setTimeout(25000);
  if (!httpsClient.connect("api.deepseek.com", 443)) {
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
  unsigned long deadline = millis() + 30000UL;
  while (httpsClient.available() == 0) {
    if (millis() > deadline) {
      outReport = "DeepSeek timeout waiting for headers.";
      httpsClient.stop();
      return false;
    }
    delay(10);
  }
  String statusLine = httpsClient.readStringUntil('\n');
  statusLine.trim();
  bool chunked = false;
  int contentLength = -1;
  while (millis() <= deadline) {
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
  StaticJsonDocument<6144> doc;
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
          if (extracted.length() > 1800) break;
        }
        answer = collapseWhitespace(extracted);
      }
    }
  }
  if (answer.length() == 0) {
    outReport = "DeepSeek returned an empty answer. " + truncateText(statusLine, 60);
    return false;
  }
  outReport = "🧠 **DeepSeek:**\n" + truncateText(answer, 1800);
  return true;
}
void handleCommand(const String& content, const String& authorId, const String& authorName,
                   const String& channelId, bool isDM)
{
  // Reply only in DMs, TARGET_CHANNEL_ID, or TARGET_CHANNEL_ID1
  if (!isDM && channelId != TARGET_CHANNEL_ID && channelId != TARGET_CHANNEL_ID1) {
    return;
  }
  String cmd = content;
  cmd.trim();

  // Extract command name for the dashboard (first word, max 14 chars)
  int spIdx = cmd.indexOf(' ');
  dashLastCmd = (spIdx > 0) ? cmd.substring(0, spIdx) : cmd;
  if (dashLastCmd.length() > 14) dashLastCmd = dashLastCmd.substring(0, 14);
  dashLastCmdMillis = millis();

  // Count this command usage for dashboard (reset happens every 24h).
  recordUserUse(authorId, authorName);

  if (cmd.startsWith("!help")) {
    String helpMsg =
      "🤖 **MiniMe Bot Commands**\n\n"
      "**👤 Public Commands:**\n"
      "• `!weather <zip>` — Fetches the weather report for a US ZIP code.\n"
      "• `!temp` — Reads the current indoor temperature sensor.\n"
      "• `!sysinfo` — Displays system diagnostics (uptime, heap, RSSI, etc.).\n"
      "• `!time` — Displays the current bot time.\n"
      "• `!news` — Space and high-tech science headlines.\n"
      "• `!physics` — Latest arXiv physics papers.\n"
      "• `!apod` — NASA Astronomy Picture of the Day.\n"
      "• `!iss` — Current International Space Station position.\n"
      "• `!ask <question>` — Asks DeepSeek (text AI reply in chat).\n"
      "• `!help` — Shows this command list.\n\n"
      "**👑 Owner-Only Commands:**\n"
      "• `!led on` / `!led off` — Controls the RGB NeoPixel LED.\n"
      "• `!set1 on` / `!set1 off` — Controls digital output pin 1.\n"
      "• `!set2 on` / `!set2 off` — Controls digital output pin 2.\n"
      "• `!servo <0-90>` — Moves the servo motor to a specific angle.\n"
      "• `!display <text>` — Writes custom text to the OLED screen.";
    sendDiscordMessage(channelId, helpMsg);
    dashLastEvent = "Help sent";
    showTransient("Help", "Command Sent");
    return;
  }
  if (cmd.startsWith("!weather")) {
    int spaceIdx = cmd.indexOf(' ');
    if (spaceIdx < 0) {
      sendDiscordMessage(channelId, "Usage: !weather <zip>");
      return;
    }
    String zip = cmd.substring(spaceIdx + 1);
    zip.trim();
    if (zip.length() < 5) {
      sendDiscordMessage(channelId, "Invalid ZIP code.");
      return;
    }
    String report;
    showTransient("Weather", "Fetching " + zip + "...");
    if (getWeather(zip, report)) {
      sendDiscordMessage(channelId, report);
      dashLastEvent = "Weather " + zip;
      showTransient("Weather", zip, "Sent");
    } else {
      sendDiscordMessage(channelId, report);
      dashLastEvent = "Weather error";
      showTransient("Weather", "Error");
    }
    return;
  }
  if (cmd.startsWith("!news")) {
    String report;
    showTransient("News", "Fetching...");
    if (getScienceNews(report)) {
      sendDiscordMessage(channelId, report);
      dashLastEvent = "News sent";
      showTransient("News", "Sent");
    } else {
      sendDiscordMessage(channelId, report);
      dashLastEvent = "News error";
      showTransient("News", "Error");
    }
    return;
  }
  if (cmd.startsWith("!physics")) {
    String report;
    showTransient("Physics", "Fetching arXiv...");
    if (getPhysicsPapers(report)) {
      sendDiscordMessage(channelId, report);
      dashLastEvent = "Physics sent";
      showTransient("Physics", "Sent");
    } else {
      sendDiscordMessage(channelId, report);
      dashLastEvent = "Physics error";
      showTransient("Physics", "Error");
    }
    return;
  }
  if (cmd.startsWith("!apod")) {
    String report;
    showTransient("APOD", "Fetching NASA...");
    if (getApod(report)) {
      sendDiscordMessage(channelId, report);
      dashLastEvent = "APOD sent";
      showTransient("APOD", "Sent");
    } else {
      sendDiscordMessage(channelId, report);
      dashLastEvent = "APOD error";
      showTransient("APOD", "Error");
    }
    return;
  }
  if (cmd.startsWith("!iss")) {
    String report;
    showTransient("ISS", "Fetching...");
    if (getIssPosition(report)) {
      sendDiscordMessage(channelId, report);
      dashLastEvent = "ISS sent";
      showTransient("ISS", "Sent");
    } else {
      sendDiscordMessage(channelId, report);
      dashLastEvent = "ISS error";
      showTransient("ISS", "Error");
    }
    return;
  }
  if (cmd.startsWith("!temp")) {
    float c, f;
    if (readTemperature(c, f)) {
      dashTempC = c;
      dashTempF = f;
      String msg = "Current Temp: " + String(c, 1) + "°C / " + String(f, 1) + "°F";
      sendDiscordMessage(channelId, msg);
      dashLastEvent = String(f, 1) + "F sent";
      showTransient("Temp", String(f, 1) + "F/" + String(c, 1) + "C");
    } else {
      sendDiscordMessage(channelId, "Temperature sensor error.");
      dashLastEvent = "Temp sensor err";
      showTransient("Temp", "Sensor error");
    }
    return;
  }
  if (cmd.startsWith("!sysinfo")) {
    sendDiscordMessage(channelId, getSystemInfo());
    dashLastEvent = "SysInfo sent";
    showTransient("SysInfo", "Sent");
    return;
  }
  if (cmd.startsWith("!time")) {
    updateLocalTime();
    String currentTime = timeClient.getFormattedTime();
    String msg = "🕒 Current Bot Time: " + currentTime;
    sendDiscordMessage(channelId, msg);
    dashLastEvent = "Time: " + currentTime;
    showTransient("Time", currentTime);
    return;
  }
  if (cmd.startsWith("!ask")) {
    int spaceIdx = cmd.indexOf(' ');
    if (spaceIdx < 0) {
      sendDiscordMessage(channelId, "Usage: !ask <question>");
      return;
    }
    String question = cmd.substring(spaceIdx + 1);
    question.trim();
    String report;
    showTransient("DeepSeek", "Thinking...", "", 30000);
    if (askDeepSeek(question, report)) {
      sendDiscordMessage(channelId, report);
      dashLastEvent = "Ask sent";
      showTransient("DeepSeek", "Sent");
    } else {
      sendDiscordMessage(channelId, report);
      dashLastEvent = "Ask error";
      showTransient("DeepSeek", "Error");
    }
    return;
  }
  if (!isOwner(authorId)) {
    if (!isDM) {
      sendDiscordMessage(channelId, "You are not allowed to use this command.");
    }
    return;
  }
  if (cmd.startsWith("!led")) {
    if (cmd.indexOf("on") > 0) {
        pixels.setPixelColor(0, pixels.Color(255, 255, 255));
        pixels.show();
        sendDiscordMessage(channelId, "LED ON");
        dashLastEvent = "LED ON";
        showTransient("LED", "ON");
    } else if (cmd.indexOf("off") > 0) {
        pixels.setPixelColor(0, pixels.Color(0, 0, 0));
        pixels.show();
        sendDiscordMessage(channelId, "LED OFF");
        dashLastEvent = "LED OFF";
        showTransient("LED", "OFF");
    } else {
        sendDiscordMessage(channelId, "Usage: !led on/off");
    }
    return;
  }
  if (cmd.startsWith("!set1")) {
    if (cmd.indexOf("on") > 0) {
      digitalWrite(PIN_SET1, HIGH);
      sendDiscordMessage(channelId, "set1 ON");
      dashLastEvent = "set1 ON";
      showTransient("set1", "ON");
    } else if (cmd.indexOf("off") > 0) {
      digitalWrite(PIN_SET1, LOW);
      sendDiscordMessage(channelId, "set1 OFF");
      dashLastEvent = "set1 OFF";
      showTransient("set1", "OFF");
    } else {
      sendDiscordMessage(channelId, "Usage: !set1 on/off");
    }
    return;
  }
  if (cmd.startsWith("!set2")) {
    if (cmd.indexOf("on") > 0) {
      digitalWrite(PIN_SET2, HIGH);
      sendDiscordMessage(channelId, "set2 ON");
      dashLastEvent = "set2 ON";
      showTransient("set2", "ON");
    } else if (cmd.indexOf("off") > 0) {
      digitalWrite(PIN_SET2, LOW);
      sendDiscordMessage(channelId, "set2 OFF");
      dashLastEvent = "set2 OFF";
      showTransient("set2", "OFF");
    } else {
      sendDiscordMessage(channelId, "Usage: !set2 on/off");
    }
    return;
  }
  if (cmd.startsWith("!servo")) {
    int spaceIdx = cmd.indexOf(' ');
    if (spaceIdx < 0) {
      sendDiscordMessage(channelId, "Usage: !servo <0-90>");
      return;
    }
    String arg = cmd.substring(spaceIdx + 1);
    arg.trim();
    int angle = arg.toInt();
    if (angle < 0 || angle > 90) {
      sendDiscordMessage(channelId, "Angle out of range. Allowed: 0-90 degrees.");
      return;
    }
    setServoAngle(angle);
    String msg = "Servo set to " + String(angle) + " degrees.";
    sendDiscordMessage(channelId, msg);
    dashLastEvent = "Servo " + String(angle) + "deg";
    showTransient("Servo", String(angle) + " deg");
    return;
  }
  if (cmd.startsWith("!display")) {
    int spaceIdx = cmd.indexOf(' ');
    if (spaceIdx < 0) {
      sendDiscordMessage(channelId, "Usage: !display <text>");
      return;
    }
    String text = cmd.substring(spaceIdx + 1);
    text.trim();
    dashLastEvent = text.length() > 14 ? text.substring(0, 14) : text;
    showTransient("Display:", text, "", 10000);
    sendDiscordMessage(channelId, "Display updated.");
    return;
  }
}
void gatewayEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      gatewayConnected = false;
      identified       = false;
      Serial.println("[GW] DISCONNECTED");
      dashLastEvent = "GW Disconnected";
      showTransient("Gateway", "Disconnected");
      break;
    case WStype_CONNECTED:
      gatewayConnected = true;
      Serial.println("[GW] CONNECTED");
      dashLastEvent = "GW Connected";
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
          JsonObject d = (*gwDoc)["d"];
          if (d["author"]["bot"] == true) return;
          String content   = d["content"].as<String>();
          String channelId = d["channel_id"].as<String>();
          String authorId  = d["author"]["id"].as<String>();
          String authorName = d["author"]["global_name"].as<String>();
          if (authorName.length() == 0) {
            authorName = d["author"]["username"].as<String>();
          }
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
void backgroundTasks() {
  unsigned long now = millis();
  // Every 4 hours: System Health Report
  if (now - lastSysInfoMillis >= SYSINFO_INTERVAL_MS || lastSysInfoMillis == 0) {
    lastSysInfoMillis = now;
    sendDiscordMessage(TARGET_CHANNEL_ID, getSystemInfo());
  }
  // Scheduled Daily Summaries (6 AM, 12 PM, 6 PM)
  updateLocalTime();
  int currentHour = timeClient.getHours();
  int currentMinute = timeClient.getMinutes();
  if ((currentHour == 6 || currentHour == 12 || currentHour == 18) && currentMinute == 0) {
    if (lastSentHour != currentHour) {
      lastSentHour = currentHour;
      float c, f;
      if (readTemperature(c, f)) {
        String report = "⏰ **Scheduled Summary (" + String(currentHour) + ":00):**\n" +
                        "• **Indoor Temp:** " + String(c, 1) + "°C / " + String(f, 1) + "°F";
        sendDiscordMessage(TARGET_CHANNEL_ID, report);
        dashLastEvent = "Sched report";
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
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  drawStatus("WiFi", "Connecting...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  drawStatus("WiFi", "Connected");
}
void connectGateway() {
  gatewayWS.beginSSL("gateway.discord.gg", 443, "/?v=10&encoding=json");
  gatewayWS.onEvent(gatewayEvent);
  gatewayWS.setReconnectInterval(5000);
}
void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.print("PSRAM size: ");
  Serial.println(ESP.getPsramSize());
  gwDoc = new DynamicJsonDocument(GW_DOC_PSRAM);
  Serial.print("GW JSON cap: ");
  Serial.println(gwDoc->capacity());
  initTrackedUsers();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  u8g2.begin();
  u8g2.setContrast(DISPLAY_CONTRAST_FULL);
  lastDisplayActivityMillis = millis();
  drawStatus("Booting...", "ESP32-S3 Discord bot");
  setupPins();
  sensors.begin();
  connectWiFi();
  timeClient.begin();
  drawStatus("Discord", "Loading users...");
  if (fetchGuildMembersAtStartup()) {
    String n0 = trackedUsers[0].userName.length() ? trackedUsers[0].userName : "ok";
    drawStatus("Users loaded", n0);
  } else {
    drawStatus("Users", "Fetch failed");
  }
  delay(1200);
  connectGateway();
  drawStatus("Ready", "Idle presence");
}
void loop() {
  gatewayWS.loop();
  if (heartbeatIntervalMs > 0 && gatewayConnected && identified) {
    unsigned long now = millis();
    if (now - lastHeartbeatMillis >= (unsigned long)heartbeatIntervalMs) {
      lastHeartbeatMillis = now;
      sendHeartbeat();
    }
  }
  backgroundTasks();
  updateDisplay();
  delay(5);
}
